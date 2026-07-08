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
#include <arrow/io/file.h>
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
    std::string type = "stdout";  // stdout | ndjson
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
    std::string service = "unknown";
    std::string source = "log";
    InputConfig input;
    OutputConfig output;
    DedupeConfig dedupe;
};

struct Options {
    std::string config_path;
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
        if (config_.output.type == "ndjson" && !config_.output.path.empty()) {
            out_file_.open(config_.output.path, std::ios::out | std::ios::app);
            if (!out_file_) throw std::runtime_error("failed to open output: " + config_.output.path);
        } else if (config_.output.type == "arrow") {
            if (config_.output.path.empty()) {
                throw std::runtime_error("arrow output requires an --output-path file");
            }
            open_arrow_writer(config_.output.path);
        }
    }

    ~InstanceRunner() {
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
    void open_arrow_writer(const std::string& path) {
        arrow_schema_ = arrow::schema({
            arrow::field("ts_ms", arrow::int64()),
            arrow::field("instance", arrow::utf8()),
            arrow::field("batch_row", arrow::int32()),
            arrow::field("ruleset_version", arrow::utf8()),
            arrow::field("matched", arrow::boolean()),
            arrow::field("decision", arrow::utf8()),
            arrow::field("score", arrow::float64()),
            arrow::field("risk_band", arrow::utf8()),
            arrow::field("winning_rule_id", arrow::utf8()),
        });
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

    void register_models() {
        for (const std::string& model : config_.models) {
            const size_t eq = model.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= model.size()) {
                throw std::runtime_error("--model must be name=path_or_s3_uri");
            }
            engine_.register_model(model.substr(0, eq), model.substr(eq + 1));
        }
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

    void evaluate(const std::vector<std::string>& records) {
        if (records.empty()) return;
        std::string ndjson;
        size_t bytes = 0;
        for (const auto& r : records) bytes += r.size() + 1;
        ndjson.reserve(bytes);
        for (const auto& r : records) {
            ndjson += r;
            ndjson += '\n';
        }
        BatchResult result = engine_.evaluate_ndjson(ndjson);
        write_decisions(result);
    }

    void write_decisions_arrow(const BatchResult& result) {
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
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays(9);
        if (!ts_b.Finish(&arrays[0]).ok() || !instance_b.Finish(&arrays[1]).ok() ||
            !batch_row_b.Finish(&arrays[2]).ok() || !version_b.Finish(&arrays[3]).ok() ||
            !matched_b.Finish(&arrays[4]).ok() || !decision_b.Finish(&arrays[5]).ok() ||
            !score_b.Finish(&arrays[6]).ok() || !risk_b.Finish(&arrays[7]).ok() ||
            !rule_b.Finish(&arrays[8]).ok()) {
            throw std::runtime_error("failed to build arrow decision batch");
        }
        auto batch = arrow::RecordBatch::Make(arrow_schema_, n, arrays);
        std::lock_guard<std::mutex> lock(out_mu_);
        arrow::Status status = arrow_writer_->WriteRecordBatch(*batch);
        if (!status.ok()) throw std::runtime_error("failed to write arrow batch: " + status.ToString());
    }

    void write_decisions(const BatchResult& result) {
        if (arrow_writer_) {
            write_decisions_arrow(result);
            return;
        }
        std::string buffer;
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
            buffer += "\"}\n";
        }
        std::lock_guard<std::mutex> lock(out_mu_);
        if (out_file_.is_open()) {
            out_file_ << buffer;
            out_file_.flush();
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
                submit_lines(split_lines(req.body));
                res.set_content("{\"ok\":true}\n", "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(std::string("{\"ok\":false,\"error\":\"") + json_escape(e.what()) + "\"}\n",
                                "application/json");
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
    DedupeWindow dedupe_;
    std::ofstream out_file_;
    std::mutex out_mu_;
    std::shared_ptr<arrow::io::OutputStream> arrow_sink_;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> arrow_writer_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
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
        else if (a == "--output") opt.single.output.type = need("--output");
        else if (a == "--output-path") opt.single.output.path = need("--output-path");
        else if (a == "--dead-letter-path") opt.single.output.dead_letter_path = need("--dead-letter-path");
        else if (a == "--model") opt.single.models.push_back(need("--model"));
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
                << "  --output stdout|ndjson|arrow  default stdout\n"
                << "  --output-path PATH          output file path (NDJSON, or Arrow IPC when --output arrow)\n"
                << "  --dead-letter-path PATH     write malformed/skipped records as NDJSON\n"
                << "  --model name=PATH|s3://...  register ONNX model, repeatable\n"
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
