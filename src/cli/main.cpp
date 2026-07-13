#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/io/stdio.h>
#include <arrow/ipc/writer.h>

#include "blazerules/engine.h"
#include "blazerules/resource_resolver.h"
#include "blazerules/simd_kernels.h"
#include "blazerules/version.h"
#include "blazerules_io/cdc.h"
#include "blazerules_io/decoder.h"
#include "blazerules_io/file_reader.h"
#ifdef BLAZERULES_IO_KAFKA
#include "blazerules_io/stream_runtime.h"
#endif

namespace {

struct CliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::string> models;
    std::vector<std::string> paths;
    std::vector<std::string> consumer_conf;
    std::vector<std::string> producer_conf;
    std::vector<std::string> positionals;
    bool use_stdin = false;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(value);
    while (std::getline(in, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

Options parse_options(int argc, char** argv, int start) {
    Options opts;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdin") {
            opts.use_stdin = true;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            opts.positionals.push_back(arg);
            continue;
        }
        std::string key = arg.substr(2);
        std::string value;
        const size_t eq = key.find('=');
        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else {
            if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
                value = "true";
            } else {
                value = argv[++i];
            }
        }
        if (key == "model") {
            opts.models.push_back(value);
        } else if (key == "path") {
            opts.paths.push_back(value);
            opts.values[key] = value;
        } else if (key == "consumer-conf") {
            opts.consumer_conf.push_back(value);
        } else if (key == "producer-conf") {
            opts.producer_conf.push_back(value);
        } else {
            opts.values[key] = value;
        }
    }
    return opts;
}

bool has(const Options& opts, const std::string& key) {
    return opts.values.find(key) != opts.values.end();
}

bool wants_help(const Options& opts) {
    return has(opts, "help") || has(opts, "h");
}

std::string get(const Options& opts, const std::string& key,
                const std::string& fallback = {}) {
    auto it = opts.values.find(key);
    return it == opts.values.end() ? fallback : it->second;
}

int get_int(const Options& opts, const std::string& key, int fallback) {
    if (!has(opts, key)) return fallback;
    return std::stoi(get(opts, key));
}

int64_t get_i64(const Options& opts, const std::string& key, int64_t fallback) {
    if (!has(opts, key)) return fallback;
    return std::stoll(get(opts, key));
}

bool truthy(const Options& opts, const std::string& key, bool fallback = false) {
    if (!has(opts, key)) return fallback;
    const std::string value = lower_copy(get(opts, key));
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

std::string read_file_bytes(const std::string& path) {
    const std::string local = blazerules::resolve_resource_to_local(path);
    std::ifstream in(local, std::ios::binary);
    if (!in) throw CliError("failed to open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string read_stdin_bytes() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

bool looks_like_json_array(std::string_view value) {
    for (char ch : value) {
        if (!std::isspace(static_cast<unsigned char>(ch))) return ch == '[';
    }
    return false;
}

void apply_aws_options(const Options& opts) {
    if (has(opts, "aws-profile")) {
        blazerules::set_aws_profile(get(opts, "aws-profile"));
    }
    if (has(opts, "aws-region")) {
        blazerules::set_aws_region(get(opts, "aws-region"));
    }
    if (has(opts, "aws-endpoint-url")) {
        blazerules::set_aws_endpoint_url(get(opts, "aws-endpoint-url"));
    }
}

std::map<std::string, std::string> parse_kv_list(const std::vector<std::string>& items,
                                                 const char* what) {
    std::map<std::string, std::string> out;
    for (const std::string& item : items) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw CliError(std::string(what) + " must be key=value: " + item);
        }
        out[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return out;
}

void set_if_absent(Options& opts, const std::string& key, const YAML::Node& node) {
    if (node && node.IsScalar() && !has(opts, key)) {
        opts.values[key] = node.as<std::string>();
    }
}

void set_list_or_scalar_if_absent(Options& opts, const std::string& key,
                                  const YAML::Node& node) {
    if (!node || has(opts, key)) return;
    if (node.IsScalar()) {
        opts.values[key] = node.as<std::string>();
        return;
    }
    if (!node.IsSequence()) return;
    std::string joined;
    for (const auto& item : node) {
        if (!item.IsScalar()) continue;
        if (!joined.empty()) joined.push_back(',');
        joined += item.as<std::string>();
    }
    if (!joined.empty()) opts.values[key] = joined;
}

void append_kv_map(std::vector<std::string>& dst, const YAML::Node& node) {
    if (!node || !node.IsMap() || !dst.empty()) return;
    for (const auto& item : node) {
        if (!item.first.IsScalar() || !item.second.IsScalar()) continue;
        dst.push_back(item.first.as<std::string>() + "=" + item.second.as<std::string>());
    }
}

void load_config_into_options(Options& opts, const std::string& path) {
    YAML::Node root = YAML::LoadFile(blazerules::resolve_resource_to_local(path));

    set_if_absent(opts, "rules", root["rules"]);

    if (YAML::Node in = root["input"]) {
        set_if_absent(opts, "input", in["type"]);
        set_if_absent(opts, "format", in["format"]);
        set_if_absent(opts, "brokers", in["brokers"]);
        set_if_absent(opts, "input-topic", in["topic"]);
        set_list_or_scalar_if_absent(opts, "input-topic", in["topics"]);
        set_if_absent(opts, "group-id", in["group_id"]);
        append_kv_map(opts.consumer_conf, in["consumer_conf"]);
        append_kv_map(opts.producer_conf, in["producer_conf"]);
        if (in["path"] && in["path"].IsScalar() && !has(opts, "path")) {
            const std::string p = in["path"].as<std::string>();
            opts.paths.push_back(p);
            opts.values["path"] = p;
        }
        set_if_absent(opts, "schema", in["schema"]);
        set_if_absent(opts, "descriptor", in["descriptor"]);
        set_if_absent(opts, "message", in["message"]);
        set_if_absent(opts, "op-field", in["op_field"]);
    }

    if (YAML::Node out = root["output"]) {
        set_if_absent(opts, "output-topic", out["topic"]);
        set_if_absent(opts, "dlq-topic", out["dlq_topic"]);
        set_if_absent(opts, "output-path", out["path"]);
        set_if_absent(opts, "output", out["mode"]);
        set_if_absent(opts, "decision-log", out["decision_log"]);
        set_if_absent(opts, "dead-letter-log", out["dead_letter_log"]);
        append_kv_map(opts.producer_conf, out["producer_conf"]);
    }

    if (YAML::Node eng = root["engine"]) {
        set_if_absent(opts, "batch-size", eng["batch_size"]);
        set_if_absent(opts, "threads", eng["threads"]);
        set_if_absent(opts, "output-detail", eng["output_detail"]);
        set_if_absent(opts, "simd-backend", eng["simd_backend"]);
        set_if_absent(opts, "ingest-error-mode", eng["ingest_error_mode"]);
        set_if_absent(opts, "type-mismatch-mode", eng["type_mismatch_mode"]);
    }

    if (YAML::Node models = root["models"]; models && models.IsSequence() && opts.models.empty()) {
        for (const auto& m : models) {
            if (m["name"] && m["path"]) {
                opts.models.push_back(m["name"].as<std::string>() + "=" +
                                      m["path"].as<std::string>());
            }
        }
    }

    if (YAML::Node aws = root["aws"]) {
        set_if_absent(opts, "aws-profile", aws["profile"]);
        set_if_absent(opts, "aws-region", aws["region"]);
        set_if_absent(opts, "aws-endpoint-url", aws["endpoint_url"]);
    }
}

void maybe_load_config(Options& opts) {
    if (has(opts, "config")) load_config_into_options(opts, get(opts, "config"));
}

EngineConfig make_engine_config(const Options& opts) {
    EngineConfig config;
    config.batch_size = get_int(opts, "batch-size", config.batch_size);
    config.eval_thread_count = get_int(opts, "threads", config.eval_thread_count);
    config.dead_letter_path = get(opts, "dead-letter-log");
    config.decision_log_path = get(opts, "decision-log");
    config.max_error_samples = get_int(opts, "max-error-samples", config.max_error_samples);
    config.simd_backend_override = get(opts, "simd-backend", config.simd_backend_override);
    const std::string detail = lower_copy(get(opts, "output-detail", "decisions"));
    if (detail == "counts") config.output_detail = EngineConfig::OUTPUT_COUNTS;
    else if (detail == "codes") config.output_detail = EngineConfig::OUTPUT_CODES;
    else if (detail == "bitmasks") config.output_detail = EngineConfig::OUTPUT_BITMASKS;
    else config.output_detail = EngineConfig::OUTPUT_DECISIONS;

    const std::string ingest = lower_copy(get(opts, "ingest-error-mode"));
    if (ingest == "hard_fail") config.ingest_error_mode = EngineConfig::HARD_FAIL;
    if (ingest == "skip_to_dead_letter") {
        config.ingest_error_mode = EngineConfig::SKIP_TO_DEAD_LETTER;
    }

    const std::string mismatch = lower_copy(get(opts, "type-mismatch-mode"));
    if (mismatch == "coerce") config.type_mismatch_mode = EngineConfig::COERCE;
    if (mismatch == "hard_fail_type") config.type_mismatch_mode = EngineConfig::HARD_FAIL_TYPE;
    return config;
}

void register_models(RuleEngine& engine, const Options& opts) {
    for (const std::string& model : opts.models) {
        const size_t eq = model.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= model.size()) {
            throw CliError("--model must be name=path_or_s3_uri");
        }
        engine.register_model(model.substr(0, eq), model.substr(eq + 1));
    }
}

void load_rules(RuleEngine& engine, const Options& opts) {
    const std::string rules = get(opts, "rules");
    if (rules.empty()) throw CliError("--rules is required");
    engine.load_rules(rules);
}

struct EvalTotals {
    int64_t batches = 0;
    int64_t records = 0;
    int64_t matched = 0;
    int64_t skipped = 0;
    int64_t processed = 0;
    BatchResult::Timing timing;
    std::map<std::string, int64_t> rule_counts;
    std::map<std::string, int64_t> error_counts;
    std::map<std::string, std::vector<int32_t>> grouped_decisions;

    bool collect_rows = false;
    std::vector<int32_t> row_index;
    std::vector<bool> row_matched;
    std::vector<std::string> row_decision;
    std::vector<double> row_score;
    std::vector<std::string> row_risk_band;
    std::vector<std::string> row_winning_rule_id;
};

void add_timing(BatchResult::Timing& dst, const BatchResult::Timing& src) {
    dst.transpose_us += src.transpose_us;
    dst.dict_encode_us += src.dict_encode_us;
    dst.window_read_us += src.window_read_us;
    dst.window_inject_us += src.window_inject_us;
    dst.window_write_us += src.window_write_us;
    dst.model_score_us += src.model_score_us;
    dst.kernel_bind_us += src.kernel_bind_us;
    dst.evaluation_us += src.evaluation_us;
    dst.result_assemble_us += src.result_assemble_us;
    dst.total_us += src.total_us;
}

void add_result(EvalTotals& totals, const BatchResult& result, int32_t row_offset) {
    ++totals.batches;
    totals.records += result.n_records;
    totals.matched += result.n_matched;
    totals.skipped += result.messages_skipped;
    totals.processed += result.messages_processed;
    add_timing(totals.timing, result.timing);
    for (const auto& [rule, count] : result.rule_match_counts) totals.rule_counts[rule] += count;
    for (const auto& [code, count] : result.error_counts) totals.error_counts[code] += count;
    for (const auto& [decision, indices] : result.grouped_decision_indices) {
        auto& out = totals.grouped_decisions[decision];
        out.reserve(out.size() + indices.size());
        for (int32_t idx : indices) out.push_back(row_offset + idx);
    }
    if (totals.collect_rows) {
        size_t matched_pos = 0;
        for (int row = 0; row < result.n_records; ++row) {
            const bool matched = matched_pos < result.matched_record_indices.size() &&
                                 result.matched_record_indices[matched_pos] == row;
            if (matched) ++matched_pos;
            const auto r = static_cast<size_t>(row);
            totals.row_index.push_back(row_offset + row);
            totals.row_matched.push_back(matched);
            totals.row_decision.push_back(r < result.decisions.size() ? result.decisions[r] : "");
            totals.row_score.push_back(r < result.scores.size() ? result.scores[r] : 0.0);
            totals.row_risk_band.push_back(r < result.risk_bands.size() ? result.risk_bands[r] : "");
            totals.row_winning_rule_id.push_back(
                r < result.winning_rule_ids.size() ? result.winning_rule_ids[r] : "");
        }
    }
}

std::ostream& output_stream(const Options& opts, std::ofstream& file) {
    const std::string path = get(opts, "output-path");
    if (path.empty()) return std::cout;
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file) throw CliError("failed to open output path: " + path);
    return file;
}

void write_decisions_jsonl(const BatchResult& result, int32_t row_offset, std::ostream& out) {
    size_t matched_pos = 0;
    for (int row = 0; row < result.n_records; ++row) {
        const bool matched = matched_pos < result.matched_record_indices.size() &&
                             result.matched_record_indices[matched_pos] == row;
        if (matched) ++matched_pos;
        out << "{\"row\":" << (row_offset + row)
            << ",\"matched\":" << (matched ? "true" : "false");
        if (row < static_cast<int>(result.decisions.size())) {
            out << ",\"decision\":\"" << json_escape(result.decisions[static_cast<size_t>(row)]) << '"';
        }
        if (row < static_cast<int>(result.scores.size())) {
            out << ",\"score\":" << result.scores[static_cast<size_t>(row)];
        }
        if (row < static_cast<int>(result.risk_bands.size())) {
            out << ",\"risk_band\":\"" << json_escape(result.risk_bands[static_cast<size_t>(row)]) << '"';
        }
        if (row < static_cast<int>(result.winning_rule_ids.size())) {
            out << ",\"winning_rule_id\":\""
                << json_escape(result.winning_rule_ids[static_cast<size_t>(row)]) << '"';
        }
        out << "}\n";
    }
}

std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    return out;
}

void write_bitmasks(const BatchResult& result, int batch_index, std::ostream& out) {
    out << "{\"batch\":" << batch_index << ",\"records\":" << result.n_records << ",\"bitmasks\":{";
    bool first = true;
    for (const auto& [rule, mask] : result.rule_bitmasks) {
        if (!first) out << ',';
        first = false;
        out << '"' << json_escape(rule) << "\":\"" << bytes_to_hex(mask) << '"';
    }
    out << "}}\n";
}

void write_grouped_decisions(const EvalTotals& totals, std::ostream& out) {
    out << "{";
    bool first_decision = true;
    for (const auto& [decision, indices] : totals.grouped_decisions) {
        if (!first_decision) out << ',';
        first_decision = false;
        out << '"' << json_escape(decision) << "\":[";
        for (size_t i = 0; i < indices.size(); ++i) {
            if (i) out << ',';
            out << indices[i];
        }
        out << ']';
    }
    out << "}\n";
}

void write_rule_counts(const EvalTotals& totals, std::ostream& out) {
    out << "{";
    bool first = true;
    for (const auto& [rule, count] : totals.rule_counts) {
        if (!first) out << ',';
        first = false;
        out << '"' << json_escape(rule) << "\":" << count;
    }
    out << "}\n";
}

void write_summary(const EvalTotals& totals, std::ostream& out) {
    out << "{"
        << "\"batches\":" << totals.batches
        << ",\"records\":" << totals.records
        << ",\"matched_records\":" << totals.matched
        << ",\"messages_processed\":" << totals.processed
        << ",\"messages_skipped\":" << totals.skipped
        << ",\"rule_match_count_entries\":" << totals.rule_counts.size()
        << ",\"timing_ms\":{"
        << "\"transpose\":" << totals.timing.transpose_us / 1000.0
        << ",\"dict_encode\":" << totals.timing.dict_encode_us / 1000.0
        << ",\"window_read\":" << totals.timing.window_read_us / 1000.0
        << ",\"window_inject\":" << totals.timing.window_inject_us / 1000.0
        << ",\"window_write\":" << totals.timing.window_write_us / 1000.0
        << ",\"model_score\":" << totals.timing.model_score_us / 1000.0
        << ",\"kernel_bind\":" << totals.timing.kernel_bind_us / 1000.0
        << ",\"evaluation\":" << totals.timing.evaluation_us / 1000.0
        << ",\"result_assemble\":" << totals.timing.result_assemble_us / 1000.0
        << ",\"total\":" << totals.timing.total_us / 1000.0
        << "},\"error_counts\":{";
    bool first = true;
    for (const auto& [code, count] : totals.error_counts) {
        if (!first) out << ',';
        first = false;
        out << '"' << json_escape(code) << "\":" << count;
    }
    out << "}}\n";
}

void check_arrow(const arrow::Status& status, const char* what) {
    if (!status.ok()) throw CliError(std::string(what) + ": " + status.ToString());
}

void write_arrow_ipc(const EvalTotals& totals, const std::string& output_path) {
    arrow::Int32Builder row_b;
    arrow::BooleanBuilder matched_b;
    arrow::StringBuilder decision_b;
    arrow::DoubleBuilder score_b;
    arrow::StringBuilder band_b;
    arrow::StringBuilder winning_b;
    const int64_t n = static_cast<int64_t>(totals.row_index.size());
    check_arrow(row_b.AppendValues(totals.row_index), "arrow build row");
    for (int i = 0; i < n; ++i) {
        check_arrow(matched_b.Append(totals.row_matched[static_cast<size_t>(i)]), "arrow build matched");
    }
    check_arrow(decision_b.AppendValues(totals.row_decision), "arrow build decision");
    check_arrow(score_b.AppendValues(totals.row_score), "arrow build score");
    check_arrow(band_b.AppendValues(totals.row_risk_band), "arrow build risk_band");
    check_arrow(winning_b.AppendValues(totals.row_winning_rule_id), "arrow build winning_rule_id");

    std::vector<std::shared_ptr<arrow::Array>> arrays(6);
    check_arrow(row_b.Finish(&arrays[0]), "arrow finish row");
    check_arrow(matched_b.Finish(&arrays[1]), "arrow finish matched");
    check_arrow(decision_b.Finish(&arrays[2]), "arrow finish decision");
    check_arrow(score_b.Finish(&arrays[3]), "arrow finish score");
    check_arrow(band_b.Finish(&arrays[4]), "arrow finish risk_band");
    check_arrow(winning_b.Finish(&arrays[5]), "arrow finish winning_rule_id");

    auto schema = arrow::schema({
        arrow::field("row", arrow::int32()),
        arrow::field("matched", arrow::boolean()),
        arrow::field("decision", arrow::utf8()),
        arrow::field("score", arrow::float64()),
        arrow::field("risk_band", arrow::utf8()),
        arrow::field("winning_rule_id", arrow::utf8()),
    });
    auto batch = arrow::RecordBatch::Make(schema, n, arrays);

    std::shared_ptr<arrow::io::OutputStream> sink;
    if (output_path.empty()) {
        sink = std::make_shared<arrow::io::StdoutStream>();
    } else {
        auto sink_res = arrow::io::FileOutputStream::Open(output_path);
        check_arrow(sink_res.status(), "open arrow output");
        sink = *sink_res;
    }
    auto writer_res = arrow::ipc::MakeStreamWriter(sink, schema);
    check_arrow(writer_res.status(), "make arrow ipc writer");
    auto writer = *writer_res;
    check_arrow(writer->WriteRecordBatch(*batch), "write arrow ipc batch");
    check_arrow(writer->Close(), "close arrow ipc writer");
    check_arrow(sink->Close(), "close arrow output");
}

using BatchHandler = void (*)(const BatchResult&, int, std::ostream&);

EvalTotals evaluate_batches(RuleEngine& engine,
                            const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                            const std::string& output,
                            std::ostream& out) {
    EvalTotals totals;
    totals.collect_rows = (output == "arrow-ipc");
    int32_t row_offset = 0;
    int batch_index = 0;
    for (const auto& batch : batches) {
        BatchResult result = engine.evaluate_batch(batch);
        if (output == "decisions-jsonl") write_decisions_jsonl(result, row_offset, out);
        if (output == "bitmasks") write_bitmasks(result, batch_index, out);
        add_result(totals, result, row_offset);
        row_offset += result.n_records;
        ++batch_index;
    }
    return totals;
}

void evaluate_batch_into(RuleEngine& engine,
                         const std::shared_ptr<arrow::RecordBatch>& batch,
                         const std::string& output,
                         std::ostream& out,
                         EvalTotals& totals,
                         int32_t& row_offset,
                         int& batch_index) {
    BatchResult result = engine.evaluate_batch(batch);
    if (output == "decisions-jsonl") write_decisions_jsonl(result, row_offset, out);
    if (output == "bitmasks") write_bitmasks(result, batch_index, out);
    add_result(totals, result, row_offset);
    row_offset += result.n_records;
    ++batch_index;
}

void evaluate_ndjson_into(RuleEngine& engine,
                          std::string_view bytes,
                          const std::string& output,
                          std::ostream& out,
                          EvalTotals& totals,
                          int32_t& row_offset,
                          int& batch_index) {
    BatchResult result = engine.evaluate_ndjson_padded(bytes);
    if (output == "decisions-jsonl") write_decisions_jsonl(result, row_offset, out);
    if (output == "bitmasks") write_bitmasks(result, batch_index, out);
    add_result(totals, result, row_offset);
    row_offset += result.n_records;
    ++batch_index;
}

EvalTotals evaluate_ndjson(RuleEngine& engine, std::string_view bytes,
                           const std::string& output, std::ostream& out) {
    EvalTotals totals;
    totals.collect_rows = (output == "arrow-ipc");
    BatchResult result = engine.evaluate_ndjson_padded(bytes);
    if (output == "decisions-jsonl") write_decisions_jsonl(result, 0, out);
    if (output == "bitmasks") write_bitmasks(result, 0, out);
    add_result(totals, result, 0);
    return totals;
}

EvalTotals evaluate_json_array(RuleEngine& engine, std::string_view bytes,
                               const std::string& output, std::ostream& out) {
    EvalTotals totals;
    totals.collect_rows = (output == "arrow-ipc");
    BatchResult result = engine.evaluate_json_array(bytes);
    if (output == "decisions-jsonl") write_decisions_jsonl(result, 0, out);
    if (output == "bitmasks") write_bitmasks(result, 0, out);
    add_result(totals, result, 0);
    return totals;
}

std::vector<std::string_view> one_frame_view(const std::string& bytes) {
    return {std::string_view(bytes.data(), bytes.size())};
}

int command_info() {
    std::cout << "{"
              << "\"version\":\"" << blazerules::VERSION << "\","
              << "\"rule_yaml_compatibility\":\"" << blazerules::RULE_YAML_COMPATIBILITY << "\","
              << "\"simd_backend\":\"" << simd_backend_name() << "\","
              << "\"cpu_features\":\"" << json_escape(cpu_features_summary()) << "\","
              << "\"features\":{"
#ifdef BLAZERULES_ENABLE_ONNX
              << "\"onnx\":true,"
#else
              << "\"onnx\":false,"
#endif
#ifdef BLAZERULES_IO_KAFKA
              << "\"kafka\":true,"
#else
              << "\"kafka\":false,"
#endif
#ifdef BLAZERULES_IO_AVRO
              << "\"avro\":true,"
#else
              << "\"avro\":false,"
#endif
#ifdef BLAZERULES_IO_PROTOBUF
              << "\"protobuf\":true,"
#else
              << "\"protobuf\":false,"
#endif
#ifdef BLAZERULES_IO_S3
              << "\"s3\":true"
#else
              << "\"s3\":false"
#endif
              << "}}\n";
    return 0;
}

int command_eval(Options opts) {
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules eval --rules rules.yaml --input FORMAT --path DATA [flags]\n\n"
            << "Inputs:\n"
            << "  ndjson, jsonl, json, json-array, debezium, arrow-ipc, arrow, parquet, csv, avro, protobuf, auto\n\n"
            << "Output modes:\n"
            << "  none, summary, decisions-jsonl, grouped-decisions, rule-counts, bitmasks, arrow-ipc\n\n"
            << "Format-specific flags:\n"
            << "  --schema PATH                 Avro schema JSON\n"
            << "  --descriptor PATH             Protobuf FileDescriptorSet\n"
            << "  --message TYPE                Protobuf message type\n"
            << "  --op-field FIELD              Debezium operation field, default __op\n\n"
            << "Runtime flags:\n"
            << "  --config PATH                 Load a unified run config (flags override it)\n"
            << "  --batch-size N --threads N --model name=PATH --output-path PATH\n"
            << "  --output-detail counts|codes|decisions|bitmasks --ingest-error-mode M --type-mismatch-mode M\n"
            << "  --decision-log PATH --dead-letter-log PATH --simd-backend auto|scalar|neon|avx2|avx512\n";
        return 0;
    }
    maybe_load_config(opts);
    apply_aws_options(opts);
    EngineConfig config = make_engine_config(opts);
    const std::string output =
        lower_copy(get(opts, "output", has(opts, "output-path") ? "decisions-jsonl" : "summary"));
    if (!has(opts, "output-detail") &&
        (output == "none" || output == "summary" || output == "rule-counts")) {
        config.output_detail = EngineConfig::OUTPUT_COUNTS;
    }
    if (output == "bitmasks") config.output_detail = EngineConfig::OUTPUT_BITMASKS;
    RuleEngine engine(config);
    register_models(engine, opts);
    load_rules(engine, opts);

    std::ofstream out_file;
    std::ostringstream discard;
    std::ostream& out = (output == "arrow-ipc" || output == "none") ? discard : output_stream(opts, out_file);

    const std::string input = lower_copy(get(opts, "input", get(opts, "format", "auto")));
    const std::string path = get(opts, "path");
    if (!opts.use_stdin && path.empty()) throw CliError("--path or --stdin is required");

    EvalTotals totals;
    if (opts.use_stdin) {
        std::string bytes = read_stdin_bytes();
        if (input == "json-array" || (input == "json" && looks_like_json_array(bytes))) {
            totals = evaluate_json_array(engine, bytes, output, out);
        } else {
            totals = evaluate_ndjson(engine, bytes, output, out);
        }
    } else if (input == "ndjson" || input == "jsonl") {
        totals.collect_rows = (output == "arrow-ipc");
        int32_t row_offset = 0;
        int batch_index = 0;
        blazerules_io::FileReadOptions read_options;
        read_options.batch_size = config.batch_size;
        read_options.ndjson_chunk_bytes = std::max<int64_t>(
            1 << 20, static_cast<int64_t>(config.batch_size) * 1024);
        blazerules_io::for_each_ndjson_chunk(path, [&](std::string_view chunk) {
            evaluate_ndjson_into(engine, chunk, output, out, totals, row_offset, batch_index);
            return true;
        }, read_options);
    } else if (input == "json" || input == "json-array") {
        std::string bytes = blazerules_io::read_ndjson_bytes(path);
        if (input == "json-array" || looks_like_json_array(bytes)) {
            totals = evaluate_json_array(engine, bytes, output, out);
        } else {
            totals = evaluate_ndjson(engine, bytes, output, out);
        }
    } else if (input == "debezium") {
        std::string bytes = blazerules_io::read_ndjson_bytes(path);
        std::vector<std::string> lines;
        std::istringstream in(bytes);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        std::vector<std::string_view> views;
        views.reserve(lines.size());
        for (const auto& row : lines) views.emplace_back(row);
        std::string ndjson = blazerules_io::unwrap_debezium(views, get(opts, "op-field", "__op"));
        totals = evaluate_ndjson(engine, ndjson, output, out);
    } else if (input == "arrow-ipc" || input == "arrow" || input == "parquet" ||
               input == "csv" || input == "auto") {
        const auto format = blazerules_io::parse_file_format(input == "arrow" ? "arrow-ipc" : input);
        totals.collect_rows = (output == "arrow-ipc");
        int32_t row_offset = 0;
        int batch_index = 0;
        blazerules_io::FileReadOptions read_options;
        read_options.batch_size = config.batch_size;
        read_options.arrow_validation = blazerules_io::ArrowIpcValidationLevel::STRUCTURAL;
        blazerules_io::for_each_record_batch(path, format, [&](const auto& batch) {
            evaluate_batch_into(engine, batch, output, out, totals, row_offset, batch_index);
            return true;
        }, read_options);
    } else if (input == "avro") {
#ifdef BLAZERULES_IO_AVRO
        const std::string schema_path = get(opts, "schema");
        if (schema_path.empty()) throw CliError("--schema is required for Avro input");
        blazerules_io::AvroDecoder decoder(read_file_bytes(schema_path));
        std::string bytes = read_file_bytes(path);
        auto batch = decoder.decode_batch(one_frame_view(bytes));
        totals = evaluate_batches(engine, {batch}, output, out);
#else
        throw CliError("this build does not include Avro support");
#endif
    } else if (input == "protobuf") {
#ifdef BLAZERULES_IO_PROTOBUF
        const std::string descriptor = get(opts, "descriptor");
        const std::string message = get(opts, "message");
        if (descriptor.empty()) throw CliError("--descriptor is required for Protobuf input");
        if (message.empty()) throw CliError("--message is required for Protobuf input");
        blazerules_io::ProtobufDecoder decoder(read_file_bytes(descriptor), message);
        std::string bytes = read_file_bytes(path);
        auto batch = decoder.decode_batch(one_frame_view(bytes));
        totals = evaluate_batches(engine, {batch}, output, out);
#else
        throw CliError("this build does not include Protobuf support");
#endif
    } else {
        throw CliError("unknown input format: " + input);
    }

    if (output == "summary") write_summary(totals, out);
    if (output == "grouped-decisions") write_grouped_decisions(totals, out);
    if (output == "rule-counts") write_rule_counts(totals, out);
    if (output == "arrow-ipc") write_arrow_ipc(totals, get(opts, "output-path"));
    return 0;
}

int command_validate(Options opts) {
    if (wants_help(opts)) {
        std::cout << "Usage: blazerules validate --rules rules.yaml [--model name=PATH] "
                     "[--sample sample.ndjson] [--config PATH]\n";
        return 0;
    }
    maybe_load_config(opts);
    apply_aws_options(opts);
    RuleEngine engine(make_engine_config(opts));
    register_models(engine, opts);
    auto report = engine.load_rules(get(opts, "rules"));
    int64_t sample_records = -1;
    int64_t sample_skipped = -1;
    const std::string sample = get(opts, "sample");
    if (!sample.empty()) {
        std::string bytes = blazerules_io::read_ndjson_bytes(sample);
        BatchResult result = engine.evaluate_ndjson_padded(bytes);
        sample_records = result.n_records;
        sample_skipped = result.messages_skipped;
    }
    std::cout << "{\"ok\":true,\"ruleset_version\":\""
              << json_escape(engine.active_rule_set_version())
              << "\",\"conflicts\":" << report.conflicts.size()
              << ",\"subsumptions\":" << report.subsumptions.size()
              << ",\"dead_rules\":" << report.dead_rules.size();
    if (sample_records >= 0) {
        std::cout << ",\"sample_records\":" << sample_records
                  << ",\"sample_skipped\":" << sample_skipped;
    }
    std::cout << "}\n";
    return 0;
}

int command_backtest(Options opts) {
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules backtest --rules-a old.yaml --rules-b new.yaml --path data.parquet [--path more.parquet]\n\n"
            << "Flags:\n"
            << "  --rules-a PATH --rules-b PATH   rulesets to compare (required)\n"
            << "  --path PATH|s3://...            Parquet history, repeatable (required)\n"
            << "  --label-column NAME             ground-truth column; adds precision/recall to the report\n"
            << "  --batch-size N --model name=PATH --config PATH\n";
        return 0;
    }
    maybe_load_config(opts);
    apply_aws_options(opts);
    RuleEngine engine(make_engine_config(opts));
    register_models(engine, opts);
    BacktestConfig config;
    config.rules_file_a = get(opts, "rules-a");
    config.rules_file_b = get(opts, "rules-b");
    config.label_column = get(opts, "label-column");
    config.batch_size = get_int(opts, "batch-size", config.batch_size);
    config.parquet_paths = opts.paths;
    if (config.parquet_paths.empty() && has(opts, "path")) config.parquet_paths.push_back(get(opts, "path"));
    if (config.rules_file_a.empty()) throw CliError("--rules-a is required");
    if (config.rules_file_b.empty()) throw CliError("--rules-b is required");
    if (config.parquet_paths.empty()) throw CliError("--path is required");
    BacktestReport report = engine.backtest(config);
    std::cout << "{\"total_records\":" << report.total_records
              << ",\"fire_rate_a\":" << report.fire_rate_a
              << ",\"fire_rate_b\":" << report.fire_rate_b
              << ",\"new_positives\":" << report.new_positives
              << ",\"lost_positives\":" << report.lost_positives
              << ",\"agreement_rate\":" << report.agreement_rate;
    if (!config.label_column.empty()) {
        std::cout << ",\"precision_a\":" << report.precision_a
                  << ",\"recall_a\":" << report.recall_a
                  << ",\"precision_b\":" << report.precision_b
                  << ",\"recall_b\":" << report.recall_b;
    }
    std::cout << "}\n";
    return 0;
}

int command_stream_kafka(Options opts) {
#ifdef BLAZERULES_IO_KAFKA
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules stream kafka --rules rules.yaml --brokers HOST --input-topic TOPIC [flags]\n\n"
            << "Payload formats:\n"
            << "  json, ndjson, debezium, arrow-ipc, avro, protobuf\n\n"
            << "Kafka flags:\n"
            << "  --group-id ID --output-topic TOPIC --dlq-topic TOPIC --batch-size N\n"
            << "  --workers N --queue-depth N --output-mode rows|grouped|none\n"
            << "  --max-messages N --max-batches N --poll-timeout-ms N --flush-timeout-ms N\n"
            << "  --flush-interval-ms N --partition-affine true|false\n"
            << "  --commit-offsets true|false --model name=PATH --config PATH\n"
            << "  --consumer-conf k=v            librdkafka consumer setting, repeatable (e.g. SASL/SSL)\n"
            << "  --producer-conf k=v            librdkafka producer setting, repeatable\n\n"
            << "Format flags:\n"
            << "  --schema PATH                 Avro schema JSON\n"
            << "  --descriptor PATH             Protobuf FileDescriptorSet\n"
            << "  --message TYPE                Protobuf message type\n"
            << "  --arrow-validation LEVEL      full, structural, or trusted\n"
            << "  --op-field FIELD              Debezium operation field, default __op\n";
        return 0;
    }
    maybe_load_config(opts);
    apply_aws_options(opts);
    const std::string output_mode = lower_copy(get(opts, "output-mode", "rows"));
    if (!has(opts, "output-detail")) {
        opts.values["output-detail"] = output_mode == "none" ? "counts" :
            (output_mode == "grouped" ? "codes" : "decisions");
    }
    RuleEngine engine(make_engine_config(opts));
    register_models(engine, opts);
    load_rules(engine, opts);

    blazerules_io::StreamRunConfig config;
    config.brokers = get(opts, "brokers");
    config.group_id = get(opts, "group-id", "blazerules");
    config.input_topics = split_csv(get(opts, "input-topic", get(opts, "input-topics")));
    config.output_topic = get(opts, "output-topic");
    config.dlq_topic = get(opts, "dlq-topic");
    config.consumer_conf = parse_kv_list(opts.consumer_conf, "--consumer-conf");
    config.producer_conf = parse_kv_list(opts.producer_conf, "--producer-conf");
    config.batch_size = get_int(opts, "batch-size", config.batch_size);
    config.worker_count = get_int(opts, "workers", config.worker_count);
    config.queue_depth = get_int(opts, "queue-depth", config.queue_depth);
    config.poll_timeout_ms = get_int(opts, "poll-timeout-ms", config.poll_timeout_ms);
    config.flush_timeout_ms = get_int(opts, "flush-timeout-ms", config.flush_timeout_ms);
    config.flush_interval_ms = get_int(opts, "flush-interval-ms", config.flush_interval_ms);
    config.max_messages = get_i64(opts, "max-messages", config.max_messages);
    config.max_batches = get_i64(opts, "max-batches", config.max_batches);
    config.commit_offsets = truthy(opts, "commit-offsets", config.commit_offsets);
    config.partition_affine = truthy(opts, "partition-affine", config.partition_affine);
    config.output_mode = output_mode;
    config.payload_format = lower_copy(get(opts, "format", "json"));
    const std::string validation = lower_copy(get(opts, "arrow-validation", "structural"));
    if (validation == "full") {
        config.arrow_validation = blazerules_io::ArrowIpcValidationLevel::FULL;
    } else if (validation == "trusted") {
        config.arrow_validation = blazerules_io::ArrowIpcValidationLevel::TRUSTED;
    } else if (validation == "structural") {
        config.arrow_validation = blazerules_io::ArrowIpcValidationLevel::STRUCTURAL;
    } else {
        throw CliError("--arrow-validation must be full, structural, or trusted");
    }
    config.debezium_op_field = get(opts, "op-field", config.debezium_op_field);
    if (has(opts, "schema")) config.avro_schema_json = read_file_bytes(get(opts, "schema"));
    if (has(opts, "descriptor")) {
        config.protobuf_descriptor_set = read_file_bytes(get(opts, "descriptor"));
    }
    config.protobuf_message_type = get(opts, "message");

    auto stats = blazerules_io::run_stream(engine, config);
    std::cout << "{\"batches\":" << stats.batches
              << ",\"messages\":" << stats.messages
              << ",\"matched\":" << stats.matched
              << ",\"emitted\":" << stats.emitted
              << ",\"eval_ms\":" << stats.eval_us / 1000.0
              << ",\"dlq_routed\":" << stats.dlq_routed
              << ",\"delivery_errors\":" << stats.delivery_errors << "}\n";
    return 0;
#else
    (void) opts;
    throw CliError("this build does not include Kafka support");
#endif
}

void print_help() {
    std::cout
        << "BlazeRules CLI\n\n"
        << "Commands:\n"
        << "  blazerules info\n"
        << "  blazerules validate --rules rules.yaml\n"
        << "  blazerules eval --rules rules.yaml --input ndjson --path events.ndjson\n"
        << "  blazerules backtest --rules-a old.yaml --rules-b new.yaml --path data.parquet\n"
        << "  blazerules stream kafka --rules rules.yaml --brokers HOST --input-topic TOPIC\n\n"
        << "Eval inputs:\n"
        << "  ndjson, jsonl, json, json-array, debezium, arrow-ipc, arrow, parquet, csv, avro, protobuf, auto\n\n"
        << "Common flags:\n"
        << "  --rules PATH|s3://...        Rules YAML\n"
        << "  --path PATH|s3://...         Input file\n"
        << "  --stdin                      Read NDJSON/JSON from stdin\n"
        << "  --config PATH                Unified run config (flags override it)\n"
        << "  --batch-size N               Batch size for file/batch inputs\n"
        << "  --model name=PATH            Register ONNX model, repeatable\n"
        << "  --output summary|decisions-jsonl|grouped-decisions|rule-counts|bitmasks|arrow-ipc\n"
        << "  --output-path PATH           Write output to file\n"
        << "  --aws-profile PROFILE        AWS profile for s3:// resources\n"
        << "  --aws-region REGION          AWS region\n"
        << "  --aws-endpoint-url URL       Custom S3 endpoint\n";
}

struct FilesystemFinalizer {
    ~FilesystemFinalizer() { blazerules_io::finalize_filesystems(); }
};

}  // namespace

int main(int argc, char** argv) {
    FilesystemFinalizer filesystem_finalizer;
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_help();
            return 0;
        }
        const std::string command = argv[1];
        if (command == "--version") {
            std::cout << blazerules::VERSION << "\n";
            return 0;
        }
        if (command == "info") return command_info();
        if (command == "eval") return command_eval(parse_options(argc, argv, 2));
        if (command == "validate") return command_validate(parse_options(argc, argv, 2));
        if (command == "backtest") return command_backtest(parse_options(argc, argv, 2));
        if (command == "stream") {
            if (argc >= 3 && std::string(argv[2]) == "kafka") {
                return command_stream_kafka(parse_options(argc, argv, 3));
            }
            throw CliError("supported stream subcommand: kafka");
        }
        if (command == "agent") {
            throw CliError("use blazerules_agent for local HTTP/stdin/file-tail agent mode");
        }
        if (command == "dashboard") {
            throw CliError("use blazerules_dashboard for the local dashboard server");
        }
        throw CliError("unknown command: " + command);
    } catch (const std::exception& e) {
        std::cerr << "blazerules: " << e.what() << "\n";
        return 2;
    }
}
