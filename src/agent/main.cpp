#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/compression.h>
#include <httplib.h>
#include <simdjson.h>
#include <yaml-cpp/yaml.h>

#include "blazerules/engine.h"
#include "blazerules/resource_resolver.h"
#include "blazerules/version.h"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

    bool try_push(T value) {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || values_.size() >= capacity_) return false;
        values_.push_back(std::move(value));
        cv_.notify_one();
        return true;
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return closed_ || !values_.empty(); });
        if (values_.empty()) return false;
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    size_t capacity_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<T> values_;
    bool closed_ = false;
};

int64_t now_ms() {
    using namespace std::chrono;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
}

bool looks_json_object(std::string_view s) {
    std::string t = trim(s);
    return t.size() >= 2 && t.front() == '{' && t.back() == '}';
}

bool looks_json_array(std::string_view s) {
    std::string t = trim(s);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

std::optional<size_t> json_value_pos(std::string_view line, std::string_view key) {
    std::string marker = "\"" + std::string(key) + "\"";
    size_t p = line.find(marker);
    if (p == std::string_view::npos) return std::nullopt;
    p = line.find(':', p + marker.size());
    if (p == std::string_view::npos) return std::nullopt;
    ++p;
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
    return p;
}

std::optional<std::string> json_scalar_string(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp || *vp >= line.size()) return std::nullopt;
    if (line[*vp] == '"') {
        size_t p = *vp + 1;
        bool escaped = false;
        std::string out;
        for (; p < line.size(); ++p) {
            char c = line[p];
            if (escaped) {
                out += c;
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return out;
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }
    size_t p = *vp;
    size_t e = p;
    while (e < line.size() && line[e] != ',' && line[e] != '}') ++e;
    return trim(line.substr(p, e - p));
}

bool body_is_ndjson(std::string_view body) {
    for (char c : body) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool is_arrow_ipc_content_type(std::string_view content_type) {
    std::string ct = lower_ascii(content_type);
    return ct.find("application/vnd.apache.arrow.stream") != std::string::npos ||
           ct.find("application/vnd.apache.arrow.file") != std::string::npos ||
           ct.find("application/x-arrow-ipc") != std::string::npos ||
           ct.find("application/x-apache-arrow") != std::string::npos;
}

bool is_ndjson_content_type(std::string_view content_type) {
    std::string ct = lower_ascii(content_type);
    return ct.find("application/x-ndjson") != std::string::npos ||
           ct.find("application/ndjson") != std::string::npos ||
           ct.find("application/jsonl") != std::string::npos;
}

bool is_avro_content_type(std::string_view content_type) {
    std::string ct = lower_ascii(content_type);
    return ct.find("avro/binary") != std::string::npos ||
           ct.find("application/avro") != std::string::npos ||
           ct.find("application/vnd.apache.avro") != std::string::npos;
}

bool is_protobuf_content_type(std::string_view content_type) {
    std::string ct = lower_ascii(content_type);
    return ct.find("application/x-protobuf") != std::string::npos ||
           ct.find("application/protobuf") != std::string::npos ||
           ct.find("application/vnd.google.protobuf") != std::string::npos;
}

// Same magic bytes as blazerules_io::looks_like_avro_ocf, inlined here so this
// binary doesn't need to link the Avro decoder (which may not even be built
// in) just to sniff 4 bytes and reject cleanly.
bool body_looks_like_avro_ocf(std::string_view body) {
    static constexpr char kMagic[4] = {'O', 'b', 'j', static_cast<char>(0x01)};
    return body.size() >= sizeof(kMagic) &&
           std::memcmp(body.data(), kMagic, sizeof(kMagic)) == 0;
}

std::string status_message(const arrow::Status& status, std::string_view context) {
    std::string out(context);
    out += ": ";
    out += status.ToString();
    return out;
}

template <typename T>
T value_or_throw(arrow::Result<T> result, std::string_view context) {
    if (!result.ok()) throw std::runtime_error(status_message(result.status(), context));
    return std::move(result).ValueOrDie();
}

void validate_arrow_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (!batch) return;
    arrow::Status status = batch->ValidateFull();
    if (!status.ok()) throw std::runtime_error(status_message(status, "arrow ipc batch validation"));
}

template <typename Fn>
uint64_t for_each_arrow_ipc_batch(std::string_view body, Fn&& fn) {
    if (body.empty()) return 0;
    auto buffer = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(body.data()),
        static_cast<int64_t>(body.size()));
    uint64_t rows = 0;

    auto input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(input);
    if (stream_reader.ok()) {
        auto reader = stream_reader.ValueOrDie();
        while (true) {
            auto batch = value_or_throw(reader->Next(), "arrow ipc stream read");
            if (!batch) break;
            if (batch->num_rows() > 0) {
                validate_arrow_batch(batch);
                rows += static_cast<uint64_t>(batch->num_rows());
                fn(batch);
            }
        }
        return rows;
    }

    input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto file_reader = arrow::ipc::RecordBatchFileReader::Open(input);
    if (!file_reader.ok()) {
        throw std::runtime_error(status_message(stream_reader.status(), "arrow ipc stream open") +
                                 "; " + status_message(file_reader.status(), "arrow ipc file open"));
    }
    auto reader = file_reader.ValueOrDie();
    int num_batches = reader->num_record_batches();
    for (int i = 0; i < num_batches; ++i) {
        auto batch = value_or_throw(reader->ReadRecordBatch(i), "arrow ipc file read");
        if (batch && batch->num_rows() > 0) {
            validate_arrow_batch(batch);
            rows += static_cast<uint64_t>(batch->num_rows());
            fn(batch);
        }
    }
    return rows;
}

std::vector<std::string> split_lines(std::string_view body) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string_view::npos) end = body.size();
        std::string line(body.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) out.push_back(std::move(line));
        start = end + 1;
    }
    return out;
}

struct InputConfig {
    std::string type = "stdin";   // stdin | file_tail | http
    std::string path;
    std::string host = "127.0.0.1";
    int port = 9480;
};

struct OutputConfig {
    std::string type = "stdout";  // stdout | ndjson | arrow | none
    std::string path;
    std::string dead_letter_path;
};

struct DedupeConfig {
    bool enabled = false;
    std::vector<std::string> key_fields;
    int64_t ttl_seconds = 86400;
};

struct InstanceConfig {
    std::string name = "default";
    std::string rules;
    std::vector<std::string> models;
    int batch_size = 2048;
    int flush_ms = 1000;
    int eval_shards = 1;
    int s3_roll_mb = 64;
    int s3_flush_seconds = 10;
    int http_threads = std::max(4u, std::thread::hardware_concurrency());
    int http_queue_depth = 256;
    int eval_queue_depth = 64;
    int sink_queue_depth = 64;
    int sink_workers = 1;
    int64_t max_request_bytes = 256LL * 1024 * 1024;
    std::string ack_mode = "durable";
    std::string service = "unknown";
    std::string source = "log";
    InputConfig input;
    OutputConfig output;
    DedupeConfig dedupe;
};

struct Options {
    std::string config_path;
    std::string aws_region;
    std::string aws_endpoint_url;
    InstanceConfig single;
};

class DedupeWindow {
public:
    explicit DedupeWindow(DedupeConfig config) : config_(std::move(config)) {}

    bool duplicate(std::string_view record) {
        if (!config_.enabled || config_.key_fields.empty()) return false;
        int64_t now = now_ms();
        if (now - last_sweep_ms_ > 1000) sweep(now);
        std::string key;
        for (const auto& field : config_.key_fields) {
            auto v = json_scalar_string(record, field);
            if (!v) return false;
            key += field;
            key += '=';
            key += *v;
            key += '|';
        }
        auto it = seen_.find(key);
        if (it != seen_.end() && it->second > now) return true;
        seen_[std::move(key)] = now + config_.ttl_seconds * 1000;
        return false;
    }

private:
    void sweep(int64_t now) {
        last_sweep_ms_ = now;
        for (auto it = seen_.begin(); it != seen_.end();) {
            if (it->second <= now) it = seen_.erase(it);
            else ++it;
        }
    }

    DedupeConfig config_;
    std::unordered_map<std::string, int64_t> seen_;
    int64_t last_sweep_ms_ = 0;
};

class InstanceRunner {
public:
    explicit InstanceRunner(InstanceConfig config)
        : config_(std::move(config)), engine_(make_engine_config(config_)), dedupe_(config_.dedupe) {
        register_models();
        if (!config_.rules.empty()) {
            engine_.load_rules(config_.rules);
        }
        build_model_columns_from_engine();
        arrow_output_ = (config_.output.type == "arrow");
        output_disabled_ = (config_.output.type == "none" || config_.output.type == "disabled");
        const bool have_path = !config_.output.path.empty();
        if (output_disabled_) {
            return;
        }
        if (arrow_output_ && !have_path) {
            throw std::runtime_error("arrow output requires an --output-path");
        }
        roll_bytes_ = static_cast<int64_t>(std::max(1, config_.s3_roll_mb)) * 1024 * 1024;
        roll_ms_ = static_cast<int64_t>(std::max(1, config_.s3_flush_seconds)) * 1000;
        if (have_path && blazerules::is_s3_uri(config_.output.path)) {
            s3_output_ = true;
            s3_prefix_ = config_.output.path;
            if (s3_prefix_.back() != '/') s3_prefix_ += '/';
            ext_ = arrow_output_ ? "arrow" : "ndjson";
            staging_dir_ = blazerules::s3_local_cache_dir(config_.output.path) + "/staging";
            std::error_code ec;
            fs::create_directories(staging_dir_, ec);
            run_id_ = now_ms();
            part_start_ms_ = run_id_;
            current_part_path_ = next_part_path();
            if (!arrow_output_) open_ndjson_part();
            s3_sync_thread_ = std::thread([this] { s3_sync_loop(); });
        } else if (!arrow_output_ && have_path) {
            ensure_parent_dir(config_.output.path);
            out_file_.open(config_.output.path, std::ios::out | std::ios::trunc);
            if (!out_file_) throw std::runtime_error("failed to open output: " + config_.output.path);
            output_inode_ = file_inode(config_.output.path);
        }
    }

    ~InstanceRunner() {
        if (s3_output_) {
            s3_stop_.store(true);
            if (s3_sync_thread_.joinable()) s3_sync_thread_.join();
            {
                std::lock_guard<std::mutex> lock(out_mu_);
                close_current_part();
            }
            // Final flush of the last rolled parts: retry so a transient failure on the
            // very last sync doesn't silently drop the tail of the decision log.
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (blazerules::s3_sync_up(staging_dir_, s3_prefix_)) break;
            }
            return;
        }
        close_arrow_writer();
    }

    void run() {
        if (config_.input.type == "http") run_http();
        else if (config_.input.type == "file_tail") run_file_tail();
        else run_stdin();
    }

private:
    struct HttpOutcome {
        int status = 200;
        std::string error;
    };

    struct HttpTask {
        std::string content_type;
        std::string body;
        size_t payload_size = 0;
        std::promise<HttpOutcome> completion;
        std::atomic<bool> completed{false};
    };

    struct SinkTask {
        std::shared_ptr<HttpTask> request;
        std::vector<BatchResult> results;
    };

    static void complete_http(const std::shared_ptr<HttpTask>& task, HttpOutcome outcome) {
        bool expected = false;
        if (task && task->completed.compare_exchange_strong(expected, true)) {
            task->completion.set_value(std::move(outcome));
        }
    }

    void build_model_columns(const BatchResult& result) {
        if (!model_columns_.empty() || result.model_outputs.empty()) return;
        std::unordered_map<std::string, int> seen;
        for (const auto& output : result.model_outputs) {
            const int occurrence = seen[output.model_name]++;
            std::string label = "model." + output.model_name;
            if (occurrence > 0) label += "#" + std::to_string(occurrence);
            model_columns_.push_back(label);
        }
    }

    std::string next_part_path() {
        std::string seq = std::to_string(part_seq_);
        while (seq.size() < 6) seq = "0" + seq;
        return staging_dir_ + "/" + config_.name + "-" + std::to_string(run_id_) + "-" + seq + "." + ext_;
    }

    std::string current_target_path() {
        return s3_output_ ? current_part_path_ : config_.output.path;
    }

    static void ensure_parent_dir(const std::string& path) {
        if (path.empty()) return;
        std::error_code ec;
        const fs::path parent = fs::path(path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
    }

    static bool path_missing(const std::string& path) {
        if (path.empty()) return false;
        std::error_code ec;
        return !fs::exists(path, ec);
    }

    static ino_t file_inode(const std::string& path) {
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
    }

    void open_ndjson_part() {
        ensure_parent_dir(current_part_path_);
        out_file_.open(current_part_path_, std::ios::out | std::ios::trunc);
        if (!out_file_) throw std::runtime_error("failed to open ndjson part: " + current_part_path_);
        output_inode_ = file_inode(current_part_path_);
    }

    void close_arrow_writer() {
        if (arrow_writer_) {
            (void) arrow_writer_->Close();
            arrow_writer_.reset();
        }
        if (arrow_sink_) {
            (void) arrow_sink_->Close();
            arrow_sink_.reset();
        }
        output_inode_ = 0;
    }

    void close_current_part() {
        if (arrow_output_) {
            close_arrow_writer();
        } else if (out_file_.is_open()) {
            out_file_.flush();
            out_file_.close();
            output_inode_ = 0;
        }
    }

    void maybe_roll() {
        if (!s3_output_) return;
        const int64_t age = now_ms() - part_start_ms_;
        if (part_bytes_ < roll_bytes_ && age < roll_ms_) return;
        if (part_bytes_ == 0) { part_start_ms_ = now_ms(); return; }
        const std::string finished = current_part_path_;
        close_current_part();
        {
            std::lock_guard<std::mutex> lk(s3_mu_);
            rolled_parts_.push_back(finished);
        }
        ++part_seq_;
        part_bytes_ = 0;
        part_start_ms_ = now_ms();
        current_part_path_ = next_part_path();
        if (!arrow_output_) open_ndjson_part();
    }

    void s3_sync_loop() {
        while (!s3_stop_.load(std::memory_order_relaxed)) {
            const int64_t step = std::max<int64_t>(50, roll_ms_ / 20);
            for (int64_t waited = 0; waited < roll_ms_ && !s3_stop_.load(std::memory_order_relaxed); waited += step) {
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
            }
            if (s3_stop_.load(std::memory_order_relaxed)) break;
            std::vector<std::string> uploaded;
            {
                std::lock_guard<std::mutex> lk(s3_mu_);
                uploaded = rolled_parts_;
            }
            if (!blazerules::s3_sync_up(staging_dir_, s3_prefix_)) continue;
            std::lock_guard<std::mutex> lk(s3_mu_);
            for (const std::string& p : uploaded) {
                std::error_code ec;
                fs::remove(p, ec);
                auto it = std::find(rolled_parts_.begin(), rolled_parts_.end(), p);
                if (it != rolled_parts_.end()) rolled_parts_.erase(it);
            }
        }
    }

    void build_arrow_schema() {
        std::vector<std::shared_ptr<arrow::Field>> fields = {
            arrow::field("ts_ms", arrow::int64()),
            arrow::field("instance", arrow::utf8()),
            arrow::field("batch_row", arrow::int32()),
            arrow::field("ruleset_version", arrow::utf8()),
            arrow::field("matched", arrow::boolean()),
            arrow::field("decision", arrow::utf8()),
            arrow::field("score", arrow::float64()),
            arrow::field("risk_band", arrow::utf8()),
            arrow::field("winning_rule_id", arrow::utf8()),
        };
        for (const std::string& column : model_columns_) {
            fields.push_back(arrow::field(column, arrow::float64()));
        }
        arrow_schema_ = arrow::schema(fields);
    }

    void open_arrow_writer(const std::string& path) {
        if (!arrow_schema_) build_arrow_schema();
        ensure_parent_dir(path);
        auto sink_result = arrow::io::FileOutputStream::Open(path);
        if (!sink_result.ok()) {
            throw std::runtime_error("failed to open arrow output: " + sink_result.status().ToString());
        }
        arrow_sink_ = *sink_result;
        arrow::ipc::IpcWriteOptions write_options = arrow::ipc::IpcWriteOptions::Defaults();
        if (arrow::util::Codec::IsAvailable(arrow::Compression::ZSTD)) {
            auto codec = arrow::util::Codec::Create(arrow::Compression::ZSTD);
            if (codec.ok()) write_options.codec = std::move(*codec);
        } else if (arrow::util::Codec::IsAvailable(arrow::Compression::LZ4_FRAME)) {
            auto codec = arrow::util::Codec::Create(arrow::Compression::LZ4_FRAME);
            if (codec.ok()) write_options.codec = std::move(*codec);
        }
        auto writer_result = arrow::ipc::MakeStreamWriter(arrow_sink_, arrow_schema_, write_options);
        if (!writer_result.ok()) {
            throw std::runtime_error("failed to open arrow writer: " + writer_result.status().ToString());
        }
        arrow_writer_ = *writer_result;
        output_inode_ = file_inode(path);
    }

    void ensure_arrow_output_open_locked() {
        const std::string path = current_target_path();
        if (path.empty()) return;
        const bool missing = path_missing(path);
        const ino_t current_inode = file_inode(path);
        const bool replaced = arrow_writer_ && current_inode != 0 && output_inode_ != 0 &&
                              current_inode != output_inode_;
        if (arrow_writer_ && !missing && !replaced) return;
        close_arrow_writer();
        if (missing && s3_output_) part_bytes_ = 0;
        open_arrow_writer(path);
    }

    void ensure_ndjson_output_open_locked() {
        const std::string path = current_target_path();
        if (path.empty()) return;
        const bool missing = path_missing(path);
        const ino_t current_inode = file_inode(path);
        const bool replaced = out_file_.is_open() && current_inode != 0 && output_inode_ != 0 &&
                              current_inode != output_inode_;
        if (out_file_.is_open() && !missing && !replaced) return;
        if (out_file_.is_open()) {
            out_file_.flush();
            out_file_.close();
            output_inode_ = 0;
        }
        if (missing && s3_output_) part_bytes_ = 0;
        ensure_parent_dir(path);
        out_file_.open(path, std::ios::out | std::ios::app);
        if (!out_file_) throw std::runtime_error("failed to reopen output: " + path);
        output_inode_ = file_inode(path);
    }

    static EngineConfig make_engine_config(const InstanceConfig& config) {
        EngineConfig engine_config;
        engine_config.output_detail =
            (config.output.type == "none" || config.output.type == "disabled")
                ? EngineConfig::OUTPUT_COUNTS
                : EngineConfig::OUTPUT_DECISIONS;
        if (!config.output.dead_letter_path.empty()) {
            engine_config.dead_letter_path = config.output.dead_letter_path;
            engine_config.ingest_error_mode = EngineConfig::SKIP_TO_DEAD_LETTER;
            engine_config.max_error_samples = std::max(config.batch_size, engine_config.max_error_samples);
        }
        return engine_config;
    }

    void register_models() { register_models_on(engine_); }

    void register_models_on(RuleEngine& engine) {
        for (const std::string& model : config_.models) {
            const size_t eq = model.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= model.size()) {
                throw std::runtime_error("--model must be name=path_or_s3_uri");
            }
            engine.register_model(model.substr(0, eq), model.substr(eq + 1));
        }
    }

    void build_model_columns_from_engine() {
        auto channels = engine_.model_channel_columns();
        if (channels.empty() || !model_columns_.empty()) return;
        std::unordered_map<std::string, int> seen;
        for (const auto& channel : channels) {
            const int occurrence = seen[channel.first]++;
            std::string label = "model." + channel.first;
            if (occurrence > 0) label += "#" + std::to_string(occurrence);
            model_columns_.push_back(label);
        }
    }

    // Build the shard pool lazily, after the first eval has bound the schema and
    // compiled the ruleset (schema binding is deferred until first eval, so window
    // channels are only known then). Only shards stateless rulesets with dedupe off.
    void maybe_init_pool() {
        if (config_.eval_shards <= 1) return;
        std::call_once(pool_once_, [this] {
            if (config_.dedupe.enabled) {
                std::cerr << "instance '" << config_.name
                          << "': --eval-shards ignored (dedupe is enabled); using 1 engine\n";
                return;
            }
            if (!engine_.schema_bound() || engine_.num_window_channels() != 0) {
                std::cerr << "instance '" << config_.name
                          << "': --eval-shards ignored (ruleset uses windows or unbound schema); using 1 engine\n";
                return;
            }
            shard_engines_ = engine_.create_shards(config_.eval_shards - 1);
            std::vector<RuleEngine*> pool;
            for (auto& shard : shard_engines_) {
                register_models_on(*shard);
                pool.push_back(shard.get());
            }
            pool.push_back(&engine_);
            {
                std::lock_guard<std::mutex> lk(pool_mu_);
                free_engines_ = std::move(pool);
                pool_built_ = true;
            }
            std::cerr << "instance '" << config_.name << "': eval sharding ON with "
                      << config_.eval_shards << " engines (stateless ruleset)\n";
        });
    }

    RuleEngine* acquire_engine(bool& pooled) {
        std::unique_lock<std::mutex> lk(pool_mu_);
        if (!pool_built_) {
            pooled = false;
            return &engine_;
        }
        pool_cv_.wait(lk, [this] { return !free_engines_.empty(); });
        RuleEngine* e = free_engines_.back();
        free_engines_.pop_back();
        pooled = true;
        return e;
    }

    bool pool_is_built() {
        std::lock_guard<std::mutex> lock(pool_mu_);
        return pool_built_;
    }

    // Only engines actually taken from the pool are returned to it, so an engine
    // acquired in the pre-build window (before the pool existed) is never double-listed.
    void release_engine(RuleEngine* e, bool pooled) {
        if (!pooled) return;
        {
            std::lock_guard<std::mutex> lk(pool_mu_);
            free_engines_.push_back(e);
        }
        pool_cv_.notify_one();
    }

    std::string canonicalize(std::string_view line) {
        std::string t = trim(line);
        if (looks_json_object(t)) return t;
        std::string out;
        out.reserve(t.size() + 192);
        out += "{\"ts_ms\":";
        out += std::to_string(now_ms());
        out += ",\"source\":\"";
        out += json_escape(config_.source);
        out += "\",\"service\":\"";
        out += json_escape(config_.service);
        out += "\",\"instance\":\"";
        out += json_escape(config_.name);
        out += "\",\"level\":\"info\",\"message\":\"";
        out += json_escape(t);
        out += "\"}";
        return out;
    }

    void submit_lines(const std::vector<std::string>& lines) {
        std::vector<std::string> records;
        records.reserve(lines.size());
        for (const auto& line : lines) {
            if (looks_json_array(line)) {
                // Flush whatever's pending first so decision order still
                // matches input order, then handle the array line on its own
                // (it may expand into many records, not the usual one).
                if (!records.empty()) {
                    evaluate(records);
                    records.clear();
                }
                evaluate_json_array_line(line);
                continue;
            }
            input_bytes_.fetch_add(line.size() + 1, std::memory_order_relaxed);
            input_records_.fetch_add(1, std::memory_order_relaxed);
            std::string rec = canonicalize(line);
            if (dedupe_.duplicate(rec)) continue;
            records.push_back(std::move(rec));
            if (static_cast<int>(records.size()) >= config_.batch_size) {
                evaluate(records);
                records.clear();
            }
        }
        if (!records.empty()) evaluate(records);
    }

    void write_input_stats(bool force = false) {
        if (config_.output.path.empty()) return;
        const int64_t now = now_ms();
        {
            std::lock_guard<std::mutex> lock(stats_mu_);
            if (!force && now - last_stats_ms_ < 1000) return;
            last_stats_ms_ = now;
        }
        std::string stats = "{\"ts_ms\":";
        stats += std::to_string(now_ms());
        stats += ",\"input_bytes\":";
        stats += std::to_string(input_bytes_.load(std::memory_order_relaxed));
        stats += ",\"input_records\":";
        stats += std::to_string(input_records_.load(std::memory_order_relaxed));
        stats += "}\n";
        std::lock_guard<std::mutex> lock(stats_mu_);
        std::ofstream out(config_.output.path + ".stats", std::ios::out | std::ios::trunc);
        if (out) out << stats;
    }

    void stats_loop() {
        while (!http_workers_stop_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            write_input_stats();
        }
        write_input_stats(true);
    }

    // Surface evaluation failures (bad rules/lookups, schema mismatch) on the agent's
    // own stderr instead of only in the per-request 500 body. Throttled via an atomic
    // timestamp so a persistently failing config is visible without flooding the log,
    // and off the evaluation hot path (only runs when an eval actually threw).
    void log_eval_error(const std::string& what) {
        const int64_t now = now_ms();
        int64_t prev = last_error_log_ms_.load(std::memory_order_relaxed);
        if (now - prev < 1000) return;
        if (!last_error_log_ms_.compare_exchange_strong(prev, now, std::memory_order_relaxed)) return;
        std::cerr << "instance '" << config_.name << "': evaluation error: " << what << "\n";
    }

    void evaluate(const std::vector<std::string>& records) {
        if (records.empty()) return;
        bool pooled = false;
        RuleEngine* e = acquire_engine(pooled);
        BatchResult result;
        try {
            result = e->evaluate_messages(records);
        } catch (...) {
            release_engine(e, pooled);
            throw;
        }
        release_engine(e, pooled);
        write_decisions(result);
        write_input_stats();
        maybe_init_pool();
    }

    // A stdin/file-tail line that's a JSON array (e.g. `[{...},{...}]`) used
    // to fail looks_json_object (leading '{') and get wrapped whole as one
    // opaque escaped-string `message` field instead of being parsed into its
    // real N records -- the same "one ingestion unit, many actual records"
    // gap as the HTTP path already handles correctly via json_array
    // sniffing. Routes it through evaluate_json_array_padded instead.
    void evaluate_json_array_line(std::string_view line) {
        std::string padded(line);
        const size_t original_size = padded.size();
        padded.resize(padded.size() + simdjson::SIMDJSON_PADDING, '\0');
        bool pooled = false;
        RuleEngine* e = acquire_engine(pooled);
        BatchResult result;
        try {
            result = e->evaluate_json_array_padded(std::string_view(padded.data(), original_size));
        } catch (...) {
            release_engine(e, pooled);
            throw;
        }
        release_engine(e, pooled);
        input_bytes_.fetch_add(original_size, std::memory_order_relaxed);
        input_records_.fetch_add(static_cast<uint64_t>(result.messages_processed),
                                 std::memory_order_relaxed);
        write_decisions(result);
        write_input_stats();
        maybe_init_pool();
    }

    void evaluate_body(const std::string& body) {
        uint64_t records = 0;
        for (char c : body) if (c == '\n') ++records;
        if (!body.empty() && body.back() != '\n') ++records;
        input_bytes_.fetch_add(body.size(), std::memory_order_relaxed);
        input_records_.fetch_add(records, std::memory_order_relaxed);
        bool pooled = false;
        RuleEngine* e = acquire_engine(pooled);
        BatchResult result;
        try {
            result = e->evaluate_ndjson(body);
        } catch (...) {
            release_engine(e, pooled);
            throw;
        }
        release_engine(e, pooled);
        write_decisions(result);
        write_input_stats();
        maybe_init_pool();
    }

    void evaluate_arrow_ipc_body(const std::string& body) {
        input_bytes_.fetch_add(body.size(), std::memory_order_relaxed);
        uint64_t records = for_each_arrow_ipc_batch(body, [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
            if (!batch || batch->num_rows() <= 0) return;
            bool pooled = false;
            RuleEngine* e = acquire_engine(pooled);
            BatchResult result;
            try {
                result = e->evaluate_record_batch(batch);
            } catch (...) {
                release_engine(e, pooled);
                throw;
            }
            release_engine(e, pooled);
            write_decisions(result);
        });
        input_records_.fetch_add(records, std::memory_order_relaxed);
        write_input_stats();
        maybe_init_pool();
    }

    std::vector<BatchResult> evaluate_http_task(const std::shared_ptr<HttpTask>& task) {
        std::vector<BatchResult> results;
        const std::string_view body(task->body.data(), task->payload_size);
        if (is_avro_content_type(task->content_type) || is_protobuf_content_type(task->content_type) ||
            body_looks_like_avro_ocf(body)) {
            // Reject cleanly instead of falling through to the raw-text path
            // below, which splits on '\n' BYTES -- for binary Avro/Protobuf
            // data that occurs at arbitrary offsets, silently mangling the
            // payload into meaningless string fields rather than erroring.
            throw std::runtime_error(
                "this agent's HTTP endpoint does not decode Avro/Protobuf payloads -- "
                "send NDJSON, JSON-array, or Arrow IPC instead (Content-Type: '" +
                task->content_type + "')");
        } else if (is_arrow_ipc_content_type(task->content_type)) {
            input_bytes_.fetch_add(body.size(), std::memory_order_relaxed);
            uint64_t records = for_each_arrow_ipc_batch(
                body, [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
                    if (!batch || batch->num_rows() <= 0) return;
                    bool pooled = false;
                    RuleEngine* e = acquire_engine(pooled);
                    try {
                        results.push_back(e->evaluate_record_batch(batch));
                    } catch (...) {
                        release_engine(e, pooled);
                        throw;
                    }
                    release_engine(e, pooled);
                });
            input_records_.fetch_add(records, std::memory_order_relaxed);
        } else if (!config_.dedupe.enabled &&
                   (is_ndjson_content_type(task->content_type) || body_is_ndjson(body))) {
            uint64_t records = 0;
            bool json_array = false;
            for (char c : body) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                json_array = c == '[';
                break;
            }
            if (json_array) {
                records = 1;
            } else {
                for (char c : body) if (c == '\n') ++records;
                if (!body.empty() && body.back() != '\n') ++records;
            }
            input_bytes_.fetch_add(body.size(), std::memory_order_relaxed);
            bool pooled = false;
            RuleEngine* e = acquire_engine(pooled);
            try {
                results.push_back(json_array
                    ? e->evaluate_json_array_padded(body)
                    : e->evaluate_ndjson_padded(body));
            } catch (...) {
                release_engine(e, pooled);
                throw;
            }
            release_engine(e, pooled);
            if (json_array && !results.empty()) {
                records = static_cast<uint64_t>(results.back().messages_processed);
            }
            input_records_.fetch_add(records, std::memory_order_relaxed);
        } else {
            std::vector<std::string> lines = split_lines(body);
            uint64_t bytes_in = 0;
            for (const auto& line : lines) bytes_in += line.size() + 1;
            input_bytes_.fetch_add(bytes_in, std::memory_order_relaxed);
            input_records_.fetch_add(lines.size(), std::memory_order_relaxed);
            std::vector<std::string> records;
            records.reserve(lines.size());
            for (const auto& line : lines) {
                std::string record = canonicalize(line);
                if (dedupe_.duplicate(record)) continue;
                records.push_back(std::move(record));
                if (static_cast<int>(records.size()) >= config_.batch_size) {
                    bool pooled = false;
                    RuleEngine* e = acquire_engine(pooled);
                    try {
                        results.push_back(e->evaluate_messages(records));
                    } catch (...) {
                        release_engine(e, pooled);
                        throw;
                    }
                    release_engine(e, pooled);
                    records.clear();
                }
            }
            if (!records.empty()) {
                bool pooled = false;
                RuleEngine* e = acquire_engine(pooled);
                try {
                    results.push_back(e->evaluate_messages(records));
                } catch (...) {
                    release_engine(e, pooled);
                    throw;
                }
                release_engine(e, pooled);
            }
        }
        maybe_init_pool();
        return results;
    }

    void http_eval_loop() {
        std::shared_ptr<HttpTask> task;
        while (http_eval_queue_->pop(task)) {
            try {
                std::unique_lock<std::mutex> serial_lock(http_serial_eval_mu_, std::defer_lock);
                if (config_.eval_shards > 1) {
                    serial_lock.lock();
                    if (pool_is_built()) serial_lock.unlock();
                }
                auto results = evaluate_http_task(task);
                if (output_disabled_) {
                    complete_http(task, {});
                    continue;
                }
                SinkTask sink{task, std::move(results)};
                if (!http_sink_queue_->try_push(std::move(sink))) {
                    complete_http(task, {503, "output queue is full"});
                    continue;
                }
                if (config_.ack_mode == "evaluated") complete_http(task, {});
            } catch (const std::exception& e) {
                log_eval_error(e.what());
                complete_http(task, {500, e.what()});
            }
        }
    }

    void http_sink_loop() {
        SinkTask task;
        while (http_sink_queue_->pop(task)) {
            try {
                for (const auto& result : task.results) write_decisions(result);
                if (config_.ack_mode != "evaluated") complete_http(task.request, {});
            } catch (const std::exception& e) {
                log_eval_error(e.what());
                if (config_.ack_mode != "evaluated") {
                    complete_http(task.request, {500, e.what()});
                }
            }
        }
    }

    void write_decisions_arrow(const BatchResult& result) {
        {
            std::lock_guard<std::mutex> lock(out_mu_);
            if (!arrow_schema_) build_arrow_schema();
        }
        const int n = result.n_records;
        std::vector<uint8_t> matched(static_cast<size_t>(std::max(n, 0)), 0);
        for (int32_t idx : result.matched_record_indices) {
            if (idx >= 0 && idx < n) matched[static_cast<size_t>(idx)] = 1;
        }
        const int64_t ts = result.evaluation_timestamp_ms ? result.evaluation_timestamp_ms : now_ms();

        arrow::Int64Builder ts_b;
        arrow::StringBuilder instance_b;
        arrow::Int32Builder batch_row_b;
        arrow::StringBuilder version_b;
        arrow::BooleanBuilder matched_b;
        arrow::StringBuilder decision_b;
        arrow::DoubleBuilder score_b;
        arrow::StringBuilder risk_b;
        arrow::StringBuilder rule_b;
        std::vector<std::unique_ptr<arrow::DoubleBuilder>> model_b;
        for (size_t c = 0; c < model_columns_.size(); ++c) {
            model_b.push_back(std::make_unique<arrow::DoubleBuilder>());
        }
        for (int i = 0; i < n; ++i) {
            (void) ts_b.Append(ts);
            (void) instance_b.Append(config_.name);
            (void) batch_row_b.Append(i);
            (void) version_b.Append(result.rule_set_version);
            (void) matched_b.Append(matched[static_cast<size_t>(i)] != 0);
            (void) decision_b.Append(i < static_cast<int>(result.decisions.size()) ? result.decisions[i] : "APPROVE");
            (void) score_b.Append(i < static_cast<int>(result.scores.size()) ? result.scores[i] : 0.0);
            (void) risk_b.Append(i < static_cast<int>(result.risk_bands.size()) ? result.risk_bands[i] : "LOW");
            (void) rule_b.Append(i < static_cast<int>(result.winning_rule_ids.size()) ? result.winning_rule_ids[i] : "");
            for (size_t c = 0; c < model_b.size(); ++c) {
                double v = (c < result.model_outputs.size() &&
                            i < static_cast<int>(result.model_outputs[c].values.size()))
                    ? static_cast<double>(result.model_outputs[c].values[static_cast<size_t>(i)])
                    : 0.0;
                (void) model_b[c]->Append(v);
            }
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays(9 + model_columns_.size());
        if (!ts_b.Finish(&arrays[0]).ok() || !instance_b.Finish(&arrays[1]).ok() ||
            !batch_row_b.Finish(&arrays[2]).ok() || !version_b.Finish(&arrays[3]).ok() ||
            !matched_b.Finish(&arrays[4]).ok() || !decision_b.Finish(&arrays[5]).ok() ||
            !score_b.Finish(&arrays[6]).ok() || !risk_b.Finish(&arrays[7]).ok() ||
            !rule_b.Finish(&arrays[8]).ok()) {
            throw std::runtime_error("failed to build arrow decision batch");
        }
        for (size_t c = 0; c < model_b.size(); ++c) {
            if (!model_b[c]->Finish(&arrays[9 + c]).ok()) {
                throw std::runtime_error("failed to build arrow model score column");
            }
        }
        auto batch = arrow::RecordBatch::Make(arrow_schema_, n, arrays);
        std::lock_guard<std::mutex> lock(out_mu_);
        ensure_arrow_output_open_locked();
        arrow::Status status = arrow_writer_->WriteRecordBatch(*batch);
        if (!status.ok()) throw std::runtime_error("failed to write arrow batch: " + status.ToString());
        const int64_t now = now_ms();
        if (now - last_flush_ms_ >= 200) {
            (void) arrow_sink_->Flush();
            last_flush_ms_ = now;
        }
        if (s3_output_) {
            auto pos = arrow_sink_->Tell();
            if (pos.ok()) part_bytes_ = *pos;
            maybe_roll();
        }
    }

    void write_decisions(const BatchResult& result) {
        if (output_disabled_) return;
        {
            std::lock_guard<std::mutex> lock(out_mu_);
            build_model_columns(result);
        }
        if (arrow_output_) {
            write_decisions_arrow(result);
            return;
        }
        thread_local std::string buffer;
        buffer.clear();
        buffer.reserve(static_cast<size_t>(result.n_records) * 224);
        std::vector<uint8_t> matched(static_cast<size_t>(std::max(result.n_records, 0)), 0);
        for (int32_t idx : result.matched_record_indices) {
            if (idx >= 0 && idx < result.n_records) matched[static_cast<size_t>(idx)] = 1;
        }
        for (int i = 0; i < result.n_records; ++i) {
            std::string decision = i < static_cast<int>(result.decisions.size()) ? result.decisions[i] : "APPROVE";
            std::string risk = i < static_cast<int>(result.risk_bands.size()) ? result.risk_bands[i] : "LOW";
            std::string rule = i < static_cast<int>(result.winning_rule_ids.size()) ? result.winning_rule_ids[i] : "";
            double score = i < static_cast<int>(result.scores.size()) ? result.scores[i] : 0.0;
            buffer += "{\"ts_ms\":";
            buffer += std::to_string(result.evaluation_timestamp_ms ? result.evaluation_timestamp_ms : now_ms());
            buffer += ",\"instance\":\"";
            buffer += json_escape(config_.name);
            buffer += "\",\"batch_row\":";
            buffer += std::to_string(i);
            buffer += ",\"ruleset_version\":\"";
            buffer += json_escape(result.rule_set_version);
            buffer += "\",\"matched\":";
            buffer += matched[static_cast<size_t>(i)] ? "true" : "false";
            buffer += ",\"decision\":\"";
            buffer += json_escape(decision);
            buffer += "\",\"score\":";
            buffer += std::to_string(score);
            buffer += ",\"risk_band\":\"";
            buffer += json_escape(risk);
            buffer += "\",\"winning_rule_id\":\"";
            buffer += json_escape(rule);
            buffer += "\"";
            const size_t model_count = std::min(model_columns_.size(), result.model_outputs.size());
            if (model_count > 0) {
                buffer += ",\"model_scores\":{";
                for (size_t c = 0; c < model_count; ++c) {
                    if (c) buffer += ",";
                    buffer += "\"";
                    buffer += json_escape(model_columns_[c]);
                    buffer += "\":";
                    double v = i < static_cast<int>(result.model_outputs[c].values.size())
                        ? static_cast<double>(result.model_outputs[c].values[static_cast<size_t>(i)])
                        : 0.0;
                    buffer += std::to_string(v);
                }
                buffer += "}";
            }
            buffer += "}\n";
        }
        std::lock_guard<std::mutex> lock(out_mu_);
        ensure_ndjson_output_open_locked();
        if (out_file_.is_open()) {
            out_file_ << buffer;
            const int64_t now = now_ms();
            if (now - last_flush_ms_ >= 200) {
                out_file_.flush();
                last_flush_ms_ = now;
            }
            if (s3_output_) {
                part_bytes_ += static_cast<int64_t>(buffer.size());
                maybe_roll();
            }
        } else {
            std::cout << buffer;
            std::cout.flush();
        }
    }

    void run_stdin() {
        std::vector<std::string> batch;
        batch.reserve(static_cast<size_t>(config_.batch_size));
        std::string line;
        auto last_flush = std::chrono::steady_clock::now();
        while (!g_stop.load() && std::getline(std::cin, line)) {
            batch.push_back(line);
            auto now = std::chrono::steady_clock::now();
            bool size_flush = static_cast<int>(batch.size()) >= config_.batch_size;
            bool time_flush = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= config_.flush_ms;
            if (size_flush || time_flush) {
                submit_lines(batch);
                batch.clear();
                last_flush = now;
            }
        }
        if (!batch.empty()) submit_lines(batch);
    }

    void run_file_tail() {
        std::ifstream in(config_.input.path);
        if (!in) throw std::runtime_error("failed to open input file: " + config_.input.path);
        in.seekg(0, std::ios::end);
        ino_t current_inode = file_inode(config_.input.path);
        std::streamoff read_pos = in.tellg();
        if (read_pos < 0) read_pos = 0;
        std::vector<std::string> batch;
        batch.reserve(static_cast<size_t>(config_.batch_size));
        auto last_flush = std::chrono::steady_clock::now();
        while (!g_stop.load()) {
            std::string line;
            bool got = false;
            while (std::getline(in, line)) {
                got = true;
                batch.push_back(line);
                if (static_cast<int>(batch.size()) >= config_.batch_size) {
                    submit_lines(batch);
                    batch.clear();
                    last_flush = std::chrono::steady_clock::now();
                }
            }
            in.clear();
            std::streamoff pos = in.tellg();
            if (pos >= 0) read_pos = pos;
            if (!got) {
                // Detect log rotation (path now points at a new inode) or in-place
                // truncation (file shorter than our read position, e.g. copytruncate).
                // In either case the old descriptor is drained; reopen and read the new
                // file from the start so we don't silently stop ingesting after rotate.
                struct stat st{};
                if (::stat(config_.input.path.c_str(), &st) == 0 &&
                    (st.st_ino != current_inode || st.st_size < read_pos)) {
                    if (!batch.empty()) {
                        submit_lines(batch);
                        batch.clear();
                        last_flush = std::chrono::steady_clock::now();
                    }
                    in.close();
                    in.open(config_.input.path);
                    current_inode = st.st_ino;
                    read_pos = 0;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            auto now = std::chrono::steady_clock::now();
            if (!batch.empty() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= config_.flush_ms) {
                submit_lines(batch);
                batch.clear();
                last_flush = now;
            }
        }
        if (!batch.empty()) submit_lines(batch);
    }

    void run_http() {
        httplib::Server server;
        server.new_task_queue = [this] {
            return new httplib::ThreadPool(
                static_cast<size_t>(std::max(1, config_.http_threads)),
                static_cast<size_t>(std::max(1, config_.http_threads)),
                static_cast<size_t>(std::max(1, config_.http_queue_depth)));
        };
        http_eval_queue_ = std::make_unique<BoundedQueue<std::shared_ptr<HttpTask>>>(
            static_cast<size_t>(std::max(1, config_.eval_queue_depth)));
        http_sink_queue_ = std::make_unique<BoundedQueue<SinkTask>>(
            static_cast<size_t>(std::max(1, config_.sink_queue_depth)));
        http_workers_stop_.store(false, std::memory_order_relaxed);
        const int eval_workers = config_.dedupe.enabled
            ? 1
            : std::max(1, config_.eval_shards);
        http_eval_threads_.reserve(static_cast<size_t>(eval_workers));
        for (int i = 0; i < eval_workers; ++i) {
            http_eval_threads_.emplace_back([this] { http_eval_loop(); });
        }
        http_sink_threads_.reserve(static_cast<size_t>(config_.sink_workers));
        for (int i = 0; i < config_.sink_workers; ++i) {
            http_sink_threads_.emplace_back([this] { http_sink_loop(); });
        }
        stats_thread_ = std::thread([this] { stats_loop(); });

        server.Post("/v1/logs", [&](const httplib::Request& req, httplib::Response& res,
                                    const httplib::ContentReader& reader) {
            auto task = std::make_shared<HttpTask>();
            task->content_type = req.get_header_value("Content-Type");
            const size_t content_length = req.get_header_value_u64("Content-Length", 0);
            if (content_length > static_cast<size_t>(config_.max_request_bytes)) {
                res.status = 413;
                res.set_content("{\"ok\":false,\"error\":\"request body is too large\"}\n",
                                "application/json");
                return;
            }
            if (content_length > 0) task->body.reserve(content_length + 64);
            bool too_large = false;
            const bool read_ok = reader([&](const char* data, size_t size) {
                if (task->body.size() + size > static_cast<size_t>(config_.max_request_bytes)) {
                    too_large = true;
                    return false;
                }
                task->body.append(data, size);
                return true;
            });
            if (!read_ok || too_large) {
                res.status = too_large ? 413 : 400;
                res.set_content("{\"ok\":false,\"error\":\"request body could not be read\"}\n",
                                "application/json");
                return;
            }
            task->payload_size = task->body.size();
            if (!is_arrow_ipc_content_type(task->content_type)) {
                task->body.resize(task->payload_size + simdjson::SIMDJSON_PADDING, '\0');
            }
            auto future = task->completion.get_future();
            if (!http_eval_queue_->try_push(task)) {
                res.status = 429;
                res.set_content("{\"ok\":false,\"error\":\"evaluation queue is full\"}\n",
                                "application/json");
                return;
            }
            HttpOutcome outcome = future.get();
            res.status = outcome.status;
            if (outcome.status == 200) {
                res.set_content("{\"ok\":true}\n", "application/json");
            } else {
                res.set_content(std::string("{\"ok\":false,\"error\":\"") +
                                json_escape(outcome.error) + "\"}\n", "application/json");
            }
        });
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"ok\":true,\"instance\":\"" + json_escape(config_.name) + "\"}\n",
                            "application/json");
        });
        std::thread stopper([&] {
            while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            server.stop();
        });
        if (config_.input.host == "0.0.0.0") {
            std::cerr << "warning: agent HTTP input has no authentication and is bound to 0.0.0.0; "
                         "run it on a trusted network or behind an authenticating reverse proxy\n";
        }
        std::cerr << "BlazeRules agent instance '" << config_.name << "' listening on http://"
                  << config_.input.host << ":" << config_.input.port << "/v1/logs\n";
        server.listen(config_.input.host, config_.input.port);
        stopper.join();
        http_eval_queue_->close();
        for (auto& thread : http_eval_threads_) {
            if (thread.joinable()) thread.join();
        }
        http_eval_threads_.clear();
        http_sink_queue_->close();
        for (auto& thread : http_sink_threads_) {
            if (thread.joinable()) thread.join();
        }
        http_sink_threads_.clear();
        http_workers_stop_.store(true, std::memory_order_relaxed);
        if (stats_thread_.joinable()) stats_thread_.join();
    }

    InstanceConfig config_;
    RuleEngine engine_;
    std::vector<std::unique_ptr<RuleEngine>> shard_engines_;
    std::mutex pool_mu_;
    std::condition_variable pool_cv_;
    std::vector<RuleEngine*> free_engines_;
    std::once_flag pool_once_;
    bool pool_built_ = false;
    DedupeWindow dedupe_;
    std::ofstream out_file_;
    std::mutex out_mu_;
    std::mutex stats_mu_;
    int64_t last_flush_ms_ = 0;
    int64_t last_stats_ms_ = 0;
    ino_t output_inode_ = 0;
    std::atomic<int64_t> last_error_log_ms_{0};
    std::atomic<uint64_t> input_bytes_{0};
    std::atomic<uint64_t> input_records_{0};
    std::shared_ptr<arrow::io::OutputStream> arrow_sink_;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> arrow_writer_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
    std::vector<std::string> model_columns_;
    bool arrow_output_ = false;
    bool output_disabled_ = false;
    bool s3_output_ = false;
    std::string s3_prefix_;
    std::string staging_dir_;
    std::string ext_;
    std::string current_part_path_;
    int64_t run_id_ = 0;
    int64_t part_seq_ = 0;
    int64_t part_start_ms_ = 0;
    int64_t part_bytes_ = 0;
    int64_t roll_bytes_ = 64 * 1024 * 1024;
    int64_t roll_ms_ = 10000;
    std::thread s3_sync_thread_;
    std::atomic<bool> s3_stop_{false};
    std::mutex s3_mu_;
    std::vector<std::string> rolled_parts_;
    std::unique_ptr<BoundedQueue<std::shared_ptr<HttpTask>>> http_eval_queue_;
    std::unique_ptr<BoundedQueue<SinkTask>> http_sink_queue_;
    std::vector<std::thread> http_eval_threads_;
    std::vector<std::thread> http_sink_threads_;
    std::thread stats_thread_;
    std::atomic<bool> http_workers_stop_{false};
    std::mutex http_serial_eval_mu_;
};

std::string str_node(const YAML::Node& n, const char* key, const std::string& fallback = {}) {
    return n[key].IsDefined() ? n[key].as<std::string>() : fallback;
}

int int_node(const YAML::Node& n, const char* key, int fallback) {
    return n[key].IsDefined() ? n[key].as<int>() : fallback;
}

bool bool_node(const YAML::Node& n, const char* key, bool fallback) {
    return n[key].IsDefined() ? n[key].as<bool>() : fallback;
}

InstanceConfig parse_instance(const YAML::Node& n) {
    InstanceConfig c;
    c.name = str_node(n, "name", "default");
    c.rules = str_node(n, "rules", "");
    c.batch_size = int_node(n, "batch_size", 2048);
    c.flush_ms = int_node(n, "flush_ms", 1000);
    c.eval_shards = int_node(n, "eval_shards", 1);
    c.s3_roll_mb = int_node(n, "s3_roll_mb", 64);
    c.s3_flush_seconds = int_node(n, "s3_flush_seconds", 10);
    c.http_threads = int_node(n, "http_threads", c.http_threads);
    c.http_queue_depth = int_node(n, "http_queue_depth", c.http_queue_depth);
    c.eval_queue_depth = int_node(n, "eval_queue_depth", c.eval_queue_depth);
    c.sink_queue_depth = int_node(n, "sink_queue_depth", c.sink_queue_depth);
    c.sink_workers = int_node(n, "sink_workers", c.sink_workers);
    if (c.sink_workers < 1) {
        throw std::runtime_error("sink_workers must be at least 1");
    }
    c.max_request_bytes = static_cast<int64_t>(int_node(n, "max_request_mb", 256)) * 1024 * 1024;
    c.ack_mode = str_node(n, "ack_mode", c.ack_mode);
    c.service = str_node(n, "service", c.name);
    c.source = str_node(n, "source", "log");

    YAML::Node input = n["input"];
    if (input.IsDefined()) {
        c.input.type = str_node(input, "type", c.input.type);
        c.input.path = str_node(input, "path", "");
        c.input.host = str_node(input, "host", c.input.host);
        c.input.port = int_node(input, "port", c.input.port);
    }

    YAML::Node output = n["output"];
    if (output.IsDefined()) {
        c.output.type = str_node(output, "type", c.output.type);
        c.output.path = str_node(output, "path", "");
        c.output.dead_letter_path = str_node(output, "dead_letter_path", "");
    }

    YAML::Node models = n["models"];
    if (models.IsSequence()) {
        for (const auto& model : models) {
            if (model.IsScalar()) {
                c.models.push_back(model.as<std::string>());
            } else if (model["name"].IsDefined() && model["path"].IsDefined()) {
                c.models.push_back(model["name"].as<std::string>() + "=" +
                                   model["path"].as<std::string>());
            }
        }
    }

    YAML::Node dedupe = n["dedupe"];
    if (dedupe.IsDefined()) {
        c.dedupe.enabled = bool_node(dedupe, "enabled", false);
        c.dedupe.ttl_seconds = int_node(dedupe, "ttl_seconds", 86400);
        YAML::Node keys = dedupe["key_fields"];
        if (keys.IsSequence()) {
            for (const auto& key : keys) c.dedupe.key_fields.push_back(key.as<std::string>());
        }
    }
    return c;
}

std::vector<InstanceConfig> load_config(const std::string& path) {
    YAML::Node root = YAML::LoadFile(blazerules::resolve_resource_to_local(path));
    std::vector<InstanceConfig> out;
    YAML::Node instances = root["instances"];
    if (instances.IsSequence()) {
        for (const auto& item : instances) out.push_back(parse_instance(item));
    } else {
        out.push_back(parse_instance(root));
    }
    return out;
}

Options parse_args(int argc, char** argv) {
    Options opt;
    opt.single.name = "default";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config") opt.config_path = need("--config");
        else if (a == "--name") opt.single.name = need("--name");
        else if (a == "--rules") opt.single.rules = need("--rules");
        else if (a == "--input") opt.single.input.type = need("--input");
        else if (a == "--path") opt.single.input.path = need("--path");
        else if (a == "--host") opt.single.input.host = need("--host");
        else if (a == "--port") opt.single.input.port = std::atoi(need("--port").c_str());
        else if (a == "--batch-size") opt.single.batch_size = std::atoi(need("--batch-size").c_str());
        else if (a == "--flush-ms") opt.single.flush_ms = std::atoi(need("--flush-ms").c_str());
        else if (a == "--eval-shards") opt.single.eval_shards = std::atoi(need("--eval-shards").c_str());
        else if (a == "--http-threads") opt.single.http_threads = std::atoi(need("--http-threads").c_str());
        else if (a == "--http-queue-depth") opt.single.http_queue_depth = std::atoi(need("--http-queue-depth").c_str());
        else if (a == "--eval-queue-depth") opt.single.eval_queue_depth = std::atoi(need("--eval-queue-depth").c_str());
        else if (a == "--sink-queue-depth") opt.single.sink_queue_depth = std::atoi(need("--sink-queue-depth").c_str());
        else if (a == "--sink-workers") opt.single.sink_workers = std::atoi(need("--sink-workers").c_str());
        else if (a == "--max-request-mb") opt.single.max_request_bytes = std::atoll(need("--max-request-mb").c_str()) * 1024 * 1024;
        else if (a == "--ack-mode") opt.single.ack_mode = lower_ascii(need("--ack-mode"));
        else if (a == "--output") opt.single.output.type = need("--output");
        else if (a == "--output-path") opt.single.output.path = need("--output-path");
        else if (a == "--dead-letter-path") opt.single.output.dead_letter_path = need("--dead-letter-path");
        else if (a == "--model") opt.single.models.push_back(need("--model"));
        else if (a == "--s3-roll-mb") opt.single.s3_roll_mb = std::atoi(need("--s3-roll-mb").c_str());
        else if (a == "--s3-flush-seconds") opt.single.s3_flush_seconds = std::atoi(need("--s3-flush-seconds").c_str());
        else if (a == "--aws-region") opt.aws_region = need("--aws-region");
        else if (a == "--aws-endpoint-url") opt.aws_endpoint_url = need("--aws-endpoint-url");
        else if (a == "--service") opt.single.service = need("--service");
        else if (a == "--source") opt.single.source = need("--source");
        else if (a == "--dedupe-key") {
            opt.single.dedupe.enabled = true;
            opt.single.dedupe.key_fields.push_back(need("--dedupe-key"));
        } else if (a == "--dedupe-ttl-seconds") {
            opt.single.dedupe.enabled = true;
            opt.single.dedupe.ttl_seconds = std::atoi(need("--dedupe-ttl-seconds").c_str());
        } else if (a == "--version") {
            std::cout << blazerules::VERSION << "\n";
            std::exit(0);
        } else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: blazerules_agent [--config config.yaml] OR single-instance flags\n\n"
                << "Single instance flags:\n"
                << "  --rules PATH|s3://...       rules YAML\n"
                << "  --input stdin|file_tail|http\n"
                << "  --path PATH                 file_tail input path\n"
                << "  --host HOST --port PORT     HTTP input bind, default 127.0.0.1:9480\n"
                << "  --batch-size N              default 2048\n"
                << "  --eval-shards N             parallel eval engines for stateless rulesets, default 1\n"
                << "  --http-threads N            bounded HTTP request workers\n"
                << "  --http-queue-depth N        queued HTTP requests before 429, default 256\n"
                << "  --eval-queue-depth N        queued evaluation batches, default 64\n"
                << "  --sink-queue-depth N        queued output batches, default 64\n"
                << "  --sink-workers N            asynchronous output serializers, default 1\n"
                << "  --max-request-mb N          maximum HTTP request body, default 256\n"
                << "  --ack-mode durable|evaluated  acknowledge after sink or evaluation\n"
                << "  --output stdout|ndjson|arrow|none  default stdout\n"
                << "  --output-path PATH|s3://...  output file, or an s3:// prefix for rolled part objects\n"
                << "  --dead-letter-path PATH     write malformed/skipped records as NDJSON\n"
                << "  --model name=PATH|s3://...  register ONNX model, repeatable\n"
                << "  --s3-roll-mb N              roll an s3 part after ~N MiB, default 64\n"
                << "  --s3-flush-seconds N        roll + upload interval for s3 output, default 10\n"
                << "  --aws-region REGION         AWS region for s3 (else AWS_REGION env)\n"
                << "  --aws-endpoint-url URL      custom S3 endpoint (else AWS_ENDPOINT_URL env)\n"
                << "  --dedupe-key FIELD          can be repeated\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            std::exit(2);
        }
    }
    if (opt.single.ack_mode != "durable" && opt.single.ack_mode != "evaluated") {
        std::cerr << "--ack-mode must be durable or evaluated\n";
        std::exit(2);
    }
    if (opt.single.sink_workers < 1) {
        std::cerr << "--sink-workers must be at least 1\n";
        std::exit(2);
    }
    if (opt.single.service == "unknown") opt.single.service = opt.single.name;
    return opt;
}

void handle_signal(int) {
    g_stop.store(true);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    try {
        Options opt = parse_args(argc, argv);
        if (!opt.aws_region.empty()) blazerules::set_aws_region(opt.aws_region);
        if (!opt.aws_endpoint_url.empty()) blazerules::set_aws_endpoint_url(opt.aws_endpoint_url);
        std::vector<InstanceConfig> configs = opt.config_path.empty()
            ? std::vector<InstanceConfig>{opt.single}
            : load_config(opt.config_path);
        if (configs.empty()) throw std::runtime_error("no agent instances configured");

        std::vector<std::thread> threads;
        std::vector<std::shared_ptr<InstanceRunner>> runners;
        for (auto& config : configs) {
            if (config.rules.empty()) throw std::runtime_error("instance '" + config.name + "' missing rules");
            auto runner = std::make_shared<InstanceRunner>(std::move(config));
            threads.emplace_back([runner] { runner->run(); });
            runners.push_back(std::move(runner));
        }
        std::cerr << "BlazeRules agent started with " << runners.size()
                  << " instance(s), version " << blazerules::VERSION << "\n";
        for (auto& t : threads) t.join();
    } catch (const std::exception& e) {
        std::cerr << "blazerules_agent: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
