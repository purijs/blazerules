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

#include <simdjson.h>

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

std::string trim_left(std::string_view value) {
    size_t i = 0;
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
    return std::string(value.substr(i));
}

std::string json_array_to_ndjson(std::string_view bytes) {
    std::string trimmed = trim_left(bytes);
    if (trimmed.empty() || trimmed.front() != '[') {
        return std::string(bytes);
    }
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(trimmed).get(doc) || doc.type() != simdjson::dom::element_type::ARRAY) {
        throw CliError("json-array input must be a top-level JSON array");
    }
    std::ostringstream out;
    for (simdjson::dom::element row : doc.get_array()) {
        out << row << '\n';
    }
    return out.str();
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

EngineConfig make_engine_config(const Options& opts) {
    EngineConfig config;
    config.batch_size = get_int(opts, "batch-size", config.batch_size);
    config.eval_thread_count = get_int(opts, "threads", config.eval_thread_count);
    config.dead_letter_path = get(opts, "dead-letter-log");
    config.decision_log_path = get(opts, "decision-log");
    config.max_error_samples = get_int(opts, "max-error-samples", config.max_error_samples);
    config.simd_backend_override = get(opts, "simd-backend", config.simd_backend_override);
    config.output_detail = lower_copy(get(opts, "output-detail", "decisions")) == "bitmasks"
        ? EngineConfig::OUTPUT_BITMASKS
        : EngineConfig::OUTPUT_DECISIONS;

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

using BatchHandler = void (*)(const BatchResult&, int, std::ostream&);

EvalTotals evaluate_batches(RuleEngine& engine,
                            const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                            const std::string& output,
                            std::ostream& out) {
    EvalTotals totals;
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

EvalTotals evaluate_ndjson(RuleEngine& engine, std::string_view bytes,
                           const std::string& output, std::ostream& out) {
    EvalTotals totals;
    BatchResult result = engine.evaluate_ndjson_padded(bytes);
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

int command_eval(const Options& opts) {
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules eval --rules rules.yaml --input FORMAT --path DATA [flags]\n\n"
            << "Inputs:\n"
            << "  ndjson, json, json-array, debezium, arrow-ipc, parquet, csv, avro, protobuf, auto\n\n"
            << "Output modes:\n"
            << "  summary, decisions-jsonl, grouped-decisions, rule-counts, bitmasks\n\n"
            << "Format-specific flags:\n"
            << "  --schema PATH                 Avro schema JSON\n"
            << "  --descriptor PATH             Protobuf FileDescriptorSet\n"
            << "  --message TYPE                Protobuf message type\n"
            << "  --op-field FIELD              Debezium operation field, default __op\n\n"
            << "Runtime flags:\n"
            << "  --batch-size N --threads N --model name=PATH --output-path PATH\n"
            << "  --decision-log PATH --dead-letter-log PATH --simd-backend auto|scalar|neon|avx2|avx512\n";
        return 0;
    }
    apply_aws_options(opts);
    EngineConfig config = make_engine_config(opts);
    const std::string output = lower_copy(get(opts, "output", "summary"));
    if (output == "bitmasks") config.output_detail = EngineConfig::OUTPUT_BITMASKS;
    RuleEngine engine(config);
    register_models(engine, opts);
    load_rules(engine, opts);

    std::ofstream out_file;
    std::ostream& out = output_stream(opts, out_file);

    const std::string input = lower_copy(get(opts, "input", get(opts, "format", "auto")));
    const std::string path = get(opts, "path");
    if (!opts.use_stdin && path.empty()) throw CliError("--path or --stdin is required");

    EvalTotals totals;
    if (opts.use_stdin) {
        std::string bytes = read_stdin_bytes();
        std::string ndjson = (input == "json-array") ? json_array_to_ndjson(bytes) : bytes;
        totals = evaluate_ndjson(engine, ndjson, output, out);
    } else if (input == "ndjson" || input == "jsonl") {
        std::string bytes = blazerules_io::read_ndjson_bytes(path);
        totals = evaluate_ndjson(engine, bytes, output, out);
    } else if (input == "json" || input == "json-array") {
        std::string bytes = blazerules_io::read_ndjson_bytes(path);
        std::string ndjson = json_array_to_ndjson(bytes);
        totals = evaluate_ndjson(engine, ndjson, output, out);
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
        auto batches = blazerules_io::read_record_batches(path, format, config.batch_size);
        totals = evaluate_batches(engine, batches, output, out);
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
    return 0;
}

int command_validate(const Options& opts) {
    if (wants_help(opts)) {
        std::cout << "Usage: blazerules validate --rules rules.yaml [--model name=PATH]\n";
        return 0;
    }
    apply_aws_options(opts);
    RuleEngine engine(make_engine_config(opts));
    register_models(engine, opts);
    auto report = engine.load_rules(get(opts, "rules"));
    std::cout << "{\"ok\":true,\"ruleset_version\":\""
              << json_escape(engine.active_rule_set_version())
              << "\",\"conflicts\":" << report.conflicts.size()
              << ",\"subsumptions\":" << report.subsumptions.size()
              << ",\"dead_rules\":" << report.dead_rules.size() << "}\n";
    return 0;
}

int command_backtest(const Options& opts) {
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules backtest --rules-a old.yaml --rules-b new.yaml --path data.parquet [--path more.parquet]\n";
        return 0;
    }
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
              << ",\"agreement_rate\":" << report.agreement_rate << "}\n";
    return 0;
}

int command_stream_kafka(const Options& opts) {
#ifdef BLAZERULES_IO_KAFKA
    if (wants_help(opts)) {
        std::cout
            << "Usage: blazerules stream kafka --rules rules.yaml --brokers HOST --input-topic TOPIC [flags]\n\n"
            << "Payload formats:\n"
            << "  json, ndjson, debezium, arrow-ipc, avro, protobuf\n\n"
            << "Kafka flags:\n"
            << "  --group-id ID --output-topic TOPIC --batch-size N --max-messages N --max-batches N\n"
            << "  --poll-timeout-ms N --flush-timeout-ms N --commit-offsets true|false\n\n"
            << "Format flags:\n"
            << "  --schema PATH                 Avro schema JSON\n"
            << "  --descriptor PATH             Protobuf FileDescriptorSet\n"
            << "  --message TYPE                Protobuf message type\n"
            << "  --op-field FIELD              Debezium operation field, default __op\n";
        return 0;
    }
    apply_aws_options(opts);
    RuleEngine engine(make_engine_config(opts));
    register_models(engine, opts);
    load_rules(engine, opts);

    blazerules_io::StreamRunConfig config;
    config.brokers = get(opts, "brokers");
    config.group_id = get(opts, "group-id", "blazerules");
    config.input_topics = split_csv(get(opts, "input-topic", get(opts, "input-topics")));
    config.output_topic = get(opts, "output-topic");
    config.batch_size = get_int(opts, "batch-size", config.batch_size);
    config.poll_timeout_ms = get_int(opts, "poll-timeout-ms", config.poll_timeout_ms);
    config.flush_timeout_ms = get_int(opts, "flush-timeout-ms", config.flush_timeout_ms);
    config.max_messages = get_i64(opts, "max-messages", config.max_messages);
    config.max_batches = get_i64(opts, "max-batches", config.max_batches);
    config.commit_offsets = truthy(opts, "commit-offsets", config.commit_offsets);
    config.payload_format = lower_copy(get(opts, "format", "json"));
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
        << "  ndjson, json, json-array, debezium, arrow-ipc, parquet, csv, avro, protobuf\n\n"
        << "Common flags:\n"
        << "  --rules PATH|s3://...        Rules YAML\n"
        << "  --path PATH|s3://...         Input file\n"
        << "  --stdin                      Read NDJSON/JSON from stdin\n"
        << "  --batch-size N               Batch size for file/batch inputs\n"
        << "  --model name=PATH            Register ONNX model, repeatable\n"
        << "  --output summary|decisions-jsonl|grouped-decisions|rule-counts|bitmasks\n"
        << "  --output-path PATH           Write output to file\n"
        << "  --aws-profile PROFILE        AWS profile for s3:// resources\n"
        << "  --aws-region REGION          AWS region\n"
        << "  --aws-endpoint-url URL       Custom S3 endpoint\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_help();
            return 0;
        }
        const std::string command = argv[1];
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
