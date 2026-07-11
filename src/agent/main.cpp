#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
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
#include <yaml-cpp/yaml.h>

#include "blazerules/engine.h"
#include "blazerules/resource_resolver.h"
#include "blazerules/version.h"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

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
        engine_.enable_metrics();
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
            out_file_.open(config_.output.path, std::ios::out | std::ios::trunc);
            if (!out_file_) throw std::runtime_error("failed to open output: " + config_.output.path);
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
        if (arrow_writer_) {
            (void) arrow_writer_->Close();
            arrow_writer_.reset();
        }
        if (arrow_sink_) {
            (void) arrow_sink_->Close();
            arrow_sink_.reset();
        }
    }

    void run() {
        if (config_.input.type == "http") run_http();
        else if (config_.input.type == "file_tail") run_file_tail();
        else run_stdin();
    }

private:
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

    void open_ndjson_part() {
        out_file_.open(current_part_path_, std::ios::out | std::ios::trunc);
        if (!out_file_) throw std::runtime_error("failed to open ndjson part: " + current_part_path_);
    }

    void close_current_part() {
        if (arrow_output_) {
            if (arrow_writer_) { (void) arrow_writer_->Close(); arrow_writer_.reset(); }
            if (arrow_sink_) { (void) arrow_sink_->Close(); arrow_sink_.reset(); }
        } else if (out_file_.is_open()) {
            out_file_.flush();
            out_file_.close();
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
    }

    static EngineConfig make_engine_config(const InstanceConfig& config) {
        EngineConfig engine_config;
        engine_config.output_detail = EngineConfig::OUTPUT_DECISIONS;
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
                shard->enable_metrics();
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
        uint64_t bytes_in = 0;
        for (const auto& line : lines) bytes_in += line.size() + 1;
        input_bytes_.fetch_add(bytes_in, std::memory_order_relaxed);
        input_records_.fetch_add(lines.size(), std::memory_order_relaxed);
        std::vector<std::string> records;
        records.reserve(lines.size());
        for (const auto& line : lines) {
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
        write_input_stats(true);
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
        write_input_stats(true);
        maybe_init_pool();
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
        if (!arrow_writer_) open_arrow_writer(current_target_path());
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

    // Return the inode of a path, or 0 if it cannot be stat'd.
    static ino_t file_inode(const std::string& path) {
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
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
        server.Post("/v1/logs", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                const std::string content_type = req.get_header_value("Content-Type");
                if (is_arrow_ipc_content_type(content_type)) {
                    evaluate_arrow_ipc_body(req.body);
                } else if (!config_.dedupe.enabled &&
                           (is_ndjson_content_type(content_type) || body_is_ndjson(req.body))) {
                    evaluate_body(req.body);
                } else {
                    submit_lines(split_lines(req.body));
                }
                res.set_content("{\"ok\":true}\n", "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(std::string("{\"ok\":false,\"error\":\"") + json_escape(e.what()) + "\"}\n",
                                "application/json");
                log_eval_error(e.what());
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
