#include "collectors.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <utility>

#include <httplib.h>

#include "json_util.h"

namespace fs = std::filesystem;

DecisionLogTailer::DecisionLogTailer(std::string path, size_t tail_lines)
    : path_(std::move(path)), tail_lines_(tail_lines) {}

DecisionState DecisionLogTailer::update(SourceStatus& status) {
    status.name = "decision_log";
    status.location = path_;
    status.configured = !path_.empty();
    if (path_.empty()) {
        status.active = false;
        return cached_;
    }
    std::error_code ec;
    status.bytes = fs::file_size(path_, ec);
    if (ec) {
        status.active = false;
        status.last_error = "file not found or not readable";
        return cached_;
    }
    status.active = true;
    status.last_error.clear();
    auto lines = read_tail_lines(path_, tail_lines_, 16 * 1024 * 1024);
    DecisionState next;
    double score_sum = 0.0;
    for (const auto& line : lines) {
        if (trim(line).empty()) continue;
        DecisionRow row;
        row.ts_ms = static_cast<int64_t>(json_number(line, "ts_ms").value_or(0));
        row.ruleset_version = json_string(line, "ruleset_version").value_or("");
        row.batch_row = static_cast<int64_t>(json_number(line, "batch_row").value_or(0));
        row.matched = json_bool(line, "matched").value_or(false);
        row.decision = json_string(line, "decision").value_or("");
        row.score = json_number(line, "score").value_or(0.0);
        row.risk_band = json_string(line, "risk_band").value_or("");
        row.winning_rule_id = json_string(line, "winning_rule_id").value_or("");
        next.rows_seen += 1;
        if (row.matched) next.matched_seen += 1;
        score_sum += row.score;
        if (!row.decision.empty()) next.decision_counts[row.decision] += 1;
        if (!row.risk_band.empty()) next.risk_band_counts[row.risk_band] += 1;
        if (!row.winning_rule_id.empty()) next.winning_rule_counts[row.winning_rule_id] += 1;
        if (!row.ruleset_version.empty()) next.ruleset_versions.insert(row.ruleset_version);
        next.recent.push_back(std::move(row));
    }
    if (next.rows_seen > 0) next.avg_score = score_sum / static_cast<double>(next.rows_seen);
    cached_ = std::move(next);
    status.last_success_ms = now_ms();
    return cached_;
}

DeadLetterTailer::DeadLetterTailer(std::string path, size_t tail_lines)
    : path_(std::move(path)), tail_lines_(tail_lines) {}

ErrorState DeadLetterTailer::update(SourceStatus& status) {
    status.name = "dead_letter_log";
    status.location = path_;
    status.configured = !path_.empty();
    if (path_.empty()) {
        status.active = false;
        return cached_;
    }
    std::error_code ec;
    status.bytes = fs::file_size(path_, ec);
    if (ec) {
        status.active = false;
        status.last_error = "file not found or not readable";
        return cached_;
    }
    status.active = true;
    status.last_error.clear();
    auto lines = read_tail_lines(path_, tail_lines_, 8 * 1024 * 1024);
    ErrorState next;
    for (const auto& line : lines) {
        if (trim(line).empty()) continue;
        ErrorRow row;
        row.ts_ms = static_cast<int64_t>(json_number(line, "ts_ms").value_or(0));
        row.code = json_string(line, "code").value_or("");
        row.message = json_string(line, "message").value_or("");
        row.source = json_string(line, "source").value_or("");
        row.row_index = static_cast<int64_t>(json_number(line, "row_index").value_or(-1));
        row.column_name = json_string(line, "column_name").value_or("");
        next.rows_seen += 1;
        if (!row.code.empty()) next.code_counts[row.code] += 1;
        next.recent.push_back(std::move(row));
    }
    cached_ = std::move(next);
    status.last_success_ms = now_ms();
    return cached_;
}

namespace {

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
    std::string error;
};

ParsedUrl parse_http_url(const std::string& url) {
    ParsedUrl out;
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        out.error = "only http:// metrics URLs are supported";
        return out;
    }
    std::string rest = url.substr(prefix.size());
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = std::atoi(hostport.substr(colon + 1).c_str());
    } else {
        out.host = hostport;
    }
    if (out.host.empty() || out.port <= 0) out.error = "invalid metrics URL";
    return out;
}

struct RulesetParseSummary {
    bool valid = false;
    std::string error;
    int64_t rule_count = 0;
    std::map<std::string, int64_t> operator_counts;
    std::map<std::string, int64_t> action_counts;
    std::map<std::string, int64_t> field_counts;
};

RulesetParseSummary summarize_rules_yaml(const std::string& text) {
    RulesetParseSummary out;
    if (trim(text).empty()) {
        out.error = "empty rules file";
        return out;
    }
    bool saw_rules = false;
    auto lines = split_text_lines(text);
    for (const auto& raw : lines) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line == "rules:" || line.rfind("rules:", 0) == 0) saw_rules = true;
        if (line.rfind("- id:", 0) == 0 || line.rfind("id:", 0) == 0) out.rule_count += 1;
        auto scan_key = [&](const char* key, std::map<std::string, int64_t>& counts) {
            std::string prefix = std::string(key) + ":";
            size_t pos = line.find(prefix);
            if (pos == std::string::npos) return;
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(line[pos - 1])) || line[pos - 1] == '_')) return;
            std::string value = strip_yaml_scalar(line.substr(pos + prefix.size()));
            if (!value.empty() && value.front() != '[' && value.front() != '{') counts[value] += 1;
        };
        scan_key("op", out.operator_counts);
        scan_key("action", out.action_counts);
        scan_key("decision", out.action_counts);
        scan_key("field", out.field_counts);
    }
    if (!saw_rules) {
        out.error = "missing top-level rules:";
        return out;
    }
    if (out.rule_count == 0) {
        out.error = "no rule ids found";
        return out;
    }
    out.valid = true;
    return out;
}

void load_rules_file(const std::string& path, std::string& yaml, bool& valid, std::string& error,
                     int64_t& rule_count, int64_t& bytes, int64_t& modified_ms,
                     std::map<std::string, int64_t>* ops,
                     std::map<std::string, int64_t>* actions,
                     std::map<std::string, int64_t>* fields) {
    std::error_code ec;
    bytes = static_cast<int64_t>(fs::file_size(path, ec));
    if (ec) {
        valid = false;
        error = "file not found or not readable";
        return;
    }
    modified_ms = file_time_ms(path);
    std::ifstream in(path);
    if (!in) {
        valid = false;
        error = "failed to open rules file";
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    yaml = ss.str();
    auto summary = summarize_rules_yaml(yaml);
    valid = summary.valid;
    error = summary.error;
    rule_count = summary.rule_count;
    if (ops) *ops = std::move(summary.operator_counts);
    if (actions) *actions = std::move(summary.action_counts);
    if (fields) *fields = std::move(summary.field_counts);
}

std::vector<DiffRow> diff_rules(const std::string& active, const std::string& candidate, size_t limit) {
    std::vector<DiffRow> out;
    if (active.empty() || candidate.empty()) return out;
    auto a = split_text_lines(active);
    auto b = split_text_lines(candidate);
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n && out.size() < limit; ++i) {
        std::string av = i < a.size() ? a[i] : "";
        std::string bv = i < b.size() ? b[i] : "";
        if (av == bv) continue;
        DiffRow row;
        row.line = static_cast<int>(i + 1);
        row.active = av;
        row.candidate = bv;
        row.kind = av.empty() ? "added" : (bv.empty() ? "removed" : "changed");
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<RulesetVersionRow> read_versions(const std::string& dir) {
    std::vector<RulesetVersionRow> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".yaml" && path.extension() != ".yml") continue;
        RulesetVersionRow row;
        row.path = path.string();
        row.bytes = static_cast<int64_t>(fs::file_size(path, ec));
        row.modified_ms = file_time_ms(path);
        std::string yaml;
        bool valid = false;
        std::string error;
        int64_t rules = 0;
        int64_t bytes = 0;
        int64_t modified = 0;
        load_rules_file(row.path, yaml, valid, error, rules, bytes, modified, nullptr, nullptr, nullptr);
        row.valid = valid;
        row.error = error;
        row.rule_count = rules;
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const RulesetVersionRow& a, const RulesetVersionRow& b) {
        return a.modified_ms > b.modified_ms;
    });
    if (out.size() > 200) out.resize(200);
    return out;
}

} // namespace

PrometheusScraper::PrometheusScraper(std::string url) : url_(std::move(url)) {}

MetricsState PrometheusScraper::update(SourceStatus& status) {
    status.name = "metrics_url";
    status.location = url_;
    status.configured = !url_.empty();
    if (url_.empty()) {
        status.active = false;
        return cached_;
    }
    ParsedUrl parsed = parse_http_url(url_);
    if (!parsed.error.empty()) {
        cached_.last_error = parsed.error;
        status.active = false;
        status.last_error = parsed.error;
        return cached_;
    }
    httplib::Client client(parsed.host, parsed.port);
    client.set_connection_timeout(0, 300000);
    client.set_read_timeout(0, 500000);
    auto res = client.Get(parsed.path);
    if (!res || res->status != 200) {
        std::string err = res ? "metrics endpoint returned HTTP " + std::to_string(res->status)
                              : "metrics endpoint unavailable";
        cached_.last_error = err;
        status.active = false;
        status.last_error = err;
        return cached_;
    }
    MetricsState next;
    std::istringstream in(res->body);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string lhs = line.substr(0, sp);
        std::string rhs = trim(std::string_view(line).substr(sp + 1));
        char* end = nullptr;
        double value = std::strtod(rhs.c_str(), &end);
        if (end == rhs.c_str()) continue;
        MetricEntry entry;
        size_t brace = lhs.find('{');
        if (brace != std::string::npos) {
            entry.name = lhs.substr(0, brace);
            size_t close = lhs.rfind('}');
            if (close != std::string::npos && close > brace) {
                entry.labels = parse_labels(std::string_view(lhs).substr(brace + 1, close - brace - 1));
            }
        } else {
            entry.name = lhs;
        }
        entry.value = value;
        next.entries.push_back(std::move(entry));
    }
    next.last_success_ms = now_ms();
    cached_ = std::move(next);
    status.active = true;
    status.bytes = res->body.size();
    status.last_error.clear();
    status.last_success_ms = cached_.last_success_ms;
    return cached_;
}

BenchmarkReader::BenchmarkReader(std::string path) : path_(std::move(path)) {}

BenchmarkState BenchmarkReader::update(SourceStatus& status) {
    status.name = "results_jsonl";
    status.location = path_;
    status.configured = !path_.empty();
    if (path_.empty()) {
        status.active = false;
        return cached_;
    }
    std::error_code ec;
    uintmax_t size = fs::file_size(path_, ec);
    if (ec) {
        status.active = false;
        status.last_error = "file not found or not readable";
        return cached_;
    }
    status.bytes = size;
    status.active = true;
    status.last_error.clear();
    auto mtime = fs::last_write_time(path_, ec);
    int64_t mtime_count = ec ? 0 : static_cast<int64_t>(mtime.time_since_epoch().count());
    if (size == last_size_ && mtime_count == last_mtime_count_ && !cached_.rows.empty()) {
        status.last_success_ms = cached_.last_success_ms;
        return cached_;
    }
    std::ifstream in(path_);
    if (!in) {
        status.active = false;
        status.last_error = "failed to open results JSONL";
        return cached_;
    }
    std::map<std::string, BenchmarkRow> latest;
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        BenchmarkRow row;
        row.format = json_string(line, "format").value_or("");
        row.status = json_string(line, "status").value_or("");
        row.error = json_string(line, "error").value_or("");
        row.target_json_bytes = static_cast<int64_t>(json_number(line, "target_json_bytes").value_or(0));
        row.actual_avg_json_bytes = static_cast<int64_t>(json_number(line, "actual_avg_json_bytes").value_or(0));
        row.records = static_cast<int64_t>(json_number(line, "records").value_or(0));
        row.batch_size = static_cast<int64_t>(json_number(line, "batch_size").value_or(0));
        row.matched_records = static_cast<int64_t>(json_number(line, "matched_records").value_or(0));
        row.rule_match_count_entries = static_cast<int64_t>(json_number(line, "rule_match_count_entries").value_or(0));
        row.engine_rps = json_number(line, "blazerules_records_per_sec").value_or(0.0);
        row.end_to_end_rps = json_number(line, "end_to_end_records_per_sec").value_or(0.0);
        row.input_mib_per_sec = json_number(line, "input_mib_per_sec").value_or(0.0);
        row.avg_batch_latency_ms = json_number(line, "avg_batch_latency_ms").value_or(0.0);
        row.p50_ms = json_number(line, "p50_batch_latency_ms").value_or(0.0);
        row.p95_ms = json_number(line, "p95_batch_latency_ms").value_or(0.0);
        row.p99_ms = json_number(line, "p99_batch_latency_ms").value_or(0.0);
        if (row.format.empty()) continue;
        std::string key = row.format + "|" + std::to_string(row.target_json_bytes) + "|" +
                          std::to_string(row.records) + "|" + std::to_string(row.batch_size);
        latest[key] = std::move(row);
    }
    BenchmarkState next;
    for (auto& [_, row] : latest) next.rows.push_back(std::move(row));
    std::sort(next.rows.begin(), next.rows.end(), [](const BenchmarkRow& a, const BenchmarkRow& b) {
        return std::tie(a.format, a.target_json_bytes, a.records, a.batch_size) <
               std::tie(b.format, b.target_json_bytes, b.records, b.batch_size);
    });
    next.last_success_ms = now_ms();
    cached_ = std::move(next);
    last_size_ = size;
    last_mtime_count_ = mtime_count;
    status.last_success_ms = cached_.last_success_ms;
    return cached_;
}

RulesetReader::RulesetReader(std::string active_path, std::string candidate_path, std::string history_dir)
    : active_path_(std::move(active_path)),
      candidate_path_(std::move(candidate_path)),
      history_dir_(std::move(history_dir)) {}

RulesetState RulesetReader::update(SourceStatus& status) {
    status.name = "ruleset";
    status.location = active_path_;
    status.configured = !active_path_.empty();
    RulesetState next;
    next.active_path = active_path_;
    next.candidate_path = candidate_path_;
    next.history_dir = history_dir_;
    next.active_configured = !active_path_.empty();
    next.candidate_configured = !candidate_path_.empty();
    if (!active_path_.empty()) {
        load_rules_file(active_path_, next.active_yaml, next.active_valid, next.active_error,
                        next.active_rule_count, next.active_bytes, next.active_modified_ms,
                        &next.operator_counts, &next.action_counts, &next.field_counts);
        status.active = next.active_valid;
        status.bytes = static_cast<uintmax_t>(std::max<int64_t>(0, next.active_bytes));
        status.last_error = next.active_error;
        if (next.active_valid) status.last_success_ms = now_ms();
    } else {
        status.active = false;
    }
    if (!candidate_path_.empty()) {
        load_rules_file(candidate_path_, next.candidate_yaml, next.candidate_valid, next.candidate_error,
                        next.candidate_rule_count, next.candidate_bytes, next.candidate_modified_ms,
                        nullptr, nullptr, nullptr);
        next.diff_rows = diff_rules(next.active_yaml, next.candidate_yaml, 500);
    }
    if (!history_dir_.empty()) next.versions = read_versions(history_dir_);
    cached_ = std::move(next);
    return cached_;
}
