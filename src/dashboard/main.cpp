#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>

#include "blazerules/version.h"

namespace fs = std::filesystem;

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    const char* hex = "0123456789abcdef";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_pair(std::string_view key, std::string_view value) {
    return "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
}

std::string json_pair(std::string_view key, double value) {
    std::ostringstream os;
    os << "\"" << json_escape(key) << "\":";
    if (std::isfinite(value)) os << std::setprecision(12) << value;
    else os << 0;
    return os.str();
}

std::string json_pair(std::string_view key, int64_t value) {
    return "\"" + json_escape(key) + "\":" + std::to_string(value);
}

std::string json_bool_pair(std::string_view key, bool value) {
    return "\"" + json_escape(key) + "\":" + (value ? "true" : "false");
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

std::string unescape_json_string(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char n = s[++i];
        switch (n) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += n; break;
        }
    }
    return out;
}

std::optional<std::string> json_string(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp || *vp >= line.size() || line[*vp] != '"') return std::nullopt;
    size_t p = *vp + 1;
    bool escaped = false;
    for (; p < line.size(); ++p) {
        char c = line[p];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return unescape_json_string(line.substr(*vp + 1, p - (*vp + 1)));
        }
    }
    return std::nullopt;
}

std::optional<double> json_number(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp) return std::nullopt;
    std::string tmp(line.substr(*vp));
    char* end = nullptr;
    double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return std::nullopt;
    return v;
}

std::optional<bool> json_bool(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp) return std::nullopt;
    auto tail = line.substr(*vp);
    if (tail.rfind("true", 0) == 0) return true;
    if (tail.rfind("false", 0) == 0) return false;
    return std::nullopt;
}

std::vector<std::string> read_tail_lines(const std::string& path, size_t max_lines, size_t max_bytes) {
    std::vector<std::string> lines;
    if (path.empty() || max_lines == 0) return lines;
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    if (ec) return lines;
    uintmax_t start = size > max_bytes ? size - max_bytes : 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return lines;
    in.seekg(static_cast<std::streamoff>(start));
    std::string line;
    if (start > 0) std::getline(in, line);
    std::deque<std::string> ring;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ring.push_back(line);
        while (ring.size() > max_lines) ring.pop_front();
    }
    lines.assign(ring.begin(), ring.end());
    return lines;
}

struct SourceStatus {
    std::string name;
    std::string location;
    bool configured = false;
    bool active = false;
    uintmax_t bytes = 0;
    int64_t last_success_ms = 0;
    std::string last_error;
};

struct DecisionRow {
    int64_t ts_ms = 0;
    std::string ruleset_version;
    int64_t batch_row = 0;
    bool matched = false;
    std::string decision;
    double score = 0.0;
    std::string risk_band;
    std::string winning_rule_id;
};

struct DecisionState {
    std::vector<DecisionRow> recent;
    std::map<std::string, int64_t> decision_counts;
    std::map<std::string, int64_t> risk_band_counts;
    std::map<std::string, int64_t> winning_rule_counts;
    std::set<std::string> ruleset_versions;
    int64_t rows_seen = 0;
    int64_t matched_seen = 0;
    double avg_score = 0.0;
};

struct ErrorRow {
    int64_t ts_ms = 0;
    std::string code;
    std::string message;
    std::string source;
    int64_t row_index = -1;
    std::string column_name;
};

struct ErrorState {
    std::vector<ErrorRow> recent;
    std::map<std::string, int64_t> code_counts;
    int64_t rows_seen = 0;
};

struct MetricEntry {
    std::string name;
    std::map<std::string, std::string> labels;
    double value = 0.0;
};

struct MetricsState {
    std::vector<MetricEntry> entries;
    std::string last_error;
    int64_t last_success_ms = 0;
};

struct BenchmarkRow {
    std::string format;
    std::string status;
    std::string error;
    int64_t target_json_bytes = 0;
    int64_t actual_avg_json_bytes = 0;
    int64_t records = 0;
    int64_t batch_size = 0;
    int64_t matched_records = 0;
    int64_t rule_match_count_entries = 0;
    double engine_rps = 0.0;
    double end_to_end_rps = 0.0;
    double input_mib_per_sec = 0.0;
    double avg_batch_latency_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

struct BenchmarkState {
    std::vector<BenchmarkRow> rows;
    std::string last_error;
    int64_t last_success_ms = 0;
};

struct RulesetVersionRow {
    std::string path;
    int64_t modified_ms = 0;
    int64_t bytes = 0;
    int64_t rule_count = 0;
    bool valid = false;
    std::string error;
};

struct DiffRow {
    int line = 0;
    std::string active;
    std::string candidate;
    std::string kind;
};

struct RulesetState {
    std::string active_path;
    std::string candidate_path;
    std::string history_dir;
    bool active_configured = false;
    bool candidate_configured = false;
    bool active_valid = false;
    bool candidate_valid = false;
    int64_t active_modified_ms = 0;
    int64_t candidate_modified_ms = 0;
    int64_t active_bytes = 0;
    int64_t candidate_bytes = 0;
    int64_t active_rule_count = 0;
    int64_t candidate_rule_count = 0;
    std::string active_error;
    std::string candidate_error;
    std::string active_yaml;
    std::string candidate_yaml;
    std::map<std::string, int64_t> operator_counts;
    std::map<std::string, int64_t> action_counts;
    std::map<std::string, int64_t> field_counts;
    std::vector<DiffRow> diff_rows;
    std::vector<RulesetVersionRow> versions;
};

struct HistoryPoint {
    int64_t ts_ms = 0;
    double rps = 0.0;
    double total_ms = 0.0;
    double evaluation_ms = 0.0;
    double transpose_ms = 0.0;
};

struct DashboardSnapshot {
    int64_t last_update_ms = 0;
    std::map<std::string, SourceStatus> sources;
    DecisionState decisions;
    ErrorState errors;
    MetricsState metrics;
    BenchmarkState benchmarks;
    RulesetState ruleset;
    std::vector<HistoryPoint> history;
    double recent_rps = 0.0;
};

class DecisionLogTailer {
public:
    DecisionLogTailer(std::string path, size_t tail_lines)
        : path_(std::move(path)), tail_lines_(tail_lines) {}

    DecisionState update(SourceStatus& status) {
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

private:
    std::string path_;
    size_t tail_lines_ = 5000;
    DecisionState cached_;
};

class DeadLetterTailer {
public:
    DeadLetterTailer(std::string path, size_t tail_lines)
        : path_(std::move(path)), tail_lines_(tail_lines) {}

    ErrorState update(SourceStatus& status) {
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

private:
    std::string path_;
    size_t tail_lines_ = 5000;
    ErrorState cached_;
};

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

std::map<std::string, std::string> parse_labels(std::string_view labels) {
    std::map<std::string, std::string> out;
    size_t p = 0;
    while (p < labels.size()) {
        while (p < labels.size() && (labels[p] == ',' || std::isspace(static_cast<unsigned char>(labels[p])))) ++p;
        size_t eq = labels.find('=', p);
        if (eq == std::string_view::npos) break;
        std::string key = std::string(labels.substr(p, eq - p));
        p = eq + 1;
        std::string value;
        if (p < labels.size() && labels[p] == '"') {
            ++p;
            bool escaped = false;
            size_t start = p;
            for (; p < labels.size(); ++p) {
                if (escaped) escaped = false;
                else if (labels[p] == '\\') escaped = true;
                else if (labels[p] == '"') break;
            }
            value = unescape_json_string(labels.substr(start, p - start));
            if (p < labels.size()) ++p;
        } else {
            size_t comma = labels.find(',', p);
            value = std::string(labels.substr(p, comma == std::string_view::npos ? labels.size() - p : comma - p));
            p = comma == std::string_view::npos ? labels.size() : comma + 1;
        }
        out[trim(key)] = value;
    }
    return out;
}

class PrometheusScraper {
public:
    explicit PrometheusScraper(std::string url) : url_(std::move(url)) {}

    MetricsState update(SourceStatus& status) {
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

private:
    std::string url_;
    MetricsState cached_;
};

class BenchmarkReader {
public:
    explicit BenchmarkReader(std::string path) : path_(std::move(path)) {}

    BenchmarkState update(SourceStatus& status) {
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

private:
    std::string path_;
    uintmax_t last_size_ = 0;
    int64_t last_mtime_count_ = 0;
    BenchmarkState cached_;
};

std::string strip_yaml_scalar(std::string value) {
    value = trim(value);
    size_t comment = value.find(" #");
    if (comment != std::string::npos) value = trim(std::string_view(value).substr(0, comment));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

int64_t file_time_ms(const fs::path& path) {
    std::error_code ec;
    auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sctp.time_since_epoch().count();
}

std::vector<std::string> split_text_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
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

class RulesetReader {
public:
    RulesetReader(std::string active_path, std::string candidate_path, std::string history_dir)
        : active_path_(std::move(active_path)),
          candidate_path_(std::move(candidate_path)),
          history_dir_(std::move(history_dir)) {}

    RulesetState update(SourceStatus& status) {
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
            load_one(active_path_, next.active_yaml, next.active_valid, next.active_error,
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
            load_one(candidate_path_, next.candidate_yaml, next.candidate_valid, next.candidate_error,
                     next.candidate_rule_count, next.candidate_bytes, next.candidate_modified_ms,
                     nullptr, nullptr, nullptr);
            next.diff_rows = diff(next.active_yaml, next.candidate_yaml, 500);
        }
        if (!history_dir_.empty()) next.versions = read_versions(history_dir_);
        cached_ = std::move(next);
        return cached_;
    }

private:
    static void load_one(const std::string& path, std::string& yaml, bool& valid, std::string& error,
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

    static std::vector<DiffRow> diff(const std::string& active, const std::string& candidate, size_t limit) {
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

    static std::vector<RulesetVersionRow> read_versions(const std::string& dir) {
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
            load_one(row.path, yaml, valid, error, rules, bytes, modified, nullptr, nullptr, nullptr);
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

    std::string active_path_;
    std::string candidate_path_;
    std::string history_dir_;
    RulesetState cached_;
};

double metric_value(const MetricsState& metrics, const std::string& name) {
    for (const auto& entry : metrics.entries) {
        if (entry.name == name && entry.labels.empty()) return entry.value;
    }
    return 0.0;
}

double histogram_mean_us(const MetricsState& metrics, const std::string& base) {
    double sum = 0.0;
    double count = 0.0;
    for (const auto& entry : metrics.entries) {
        if (entry.name == base + "_sum" && entry.labels.empty()) sum = entry.value;
        else if (entry.name == base + "_count" && entry.labels.empty()) count = entry.value;
    }
    return count > 0.0 ? sum / count : 0.0;
}

std::string map_to_json(const std::map<std::string, int64_t>& m) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) out += ",";
        first = false;
        out += json_pair(k, v);
    }
    out += "}";
    return out;
}

std::string source_json(const SourceStatus& s) {
    std::string out = "{";
    out += json_pair("name", s.name);
    out += ",";
    out += json_pair("location", s.location);
    out += ",";
    out += json_bool_pair("configured", s.configured);
    out += ",";
    out += json_bool_pair("active", s.active);
    out += ",";
    out += json_pair("bytes", static_cast<int64_t>(s.bytes));
    out += ",";
    out += json_pair("last_success_ms", s.last_success_ms);
    out += ",";
    out += json_pair("last_error", s.last_error);
    out += "}";
    return out;
}

std::string metric_entry_json(const MetricEntry& e) {
    std::string out = "{";
    out += json_pair("name", e.name);
    out += ",";
    out += json_pair("value", e.value);
    out += ",\"labels\":{";
    bool first = true;
    for (const auto& [k, v] : e.labels) {
        if (!first) out += ",";
        first = false;
        out += json_pair(k, v);
    }
    out += "}}";
    return out;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2) return values[mid];
    return (values[mid - 1] + values[mid]) / 2.0;
}

struct Options {
    std::string host = "127.0.0.1";
    int port = 9470;
    int poll_ms = 1000;
    size_t tail_lines = 5000;
    std::string decision_log;
    std::string dead_letter_log;
    std::string metrics_url;
    std::string results_jsonl;
    std::string rules_path;
    std::string candidate_rules_path;
    std::string rules_history_dir;
};

class DashboardServer {
public:
    explicit DashboardServer(Options options)
        : options_(std::move(options)),
          decision_tailer_(options_.decision_log, options_.tail_lines),
          dead_letter_tailer_(options_.dead_letter_log, options_.tail_lines),
          metrics_scraper_(options_.metrics_url),
          benchmark_reader_(options_.results_jsonl),
          ruleset_reader_(options_.rules_path, options_.candidate_rules_path, options_.rules_history_dir) {}

    void start() {
        stop_.store(false);
        refresh_once();
        worker_ = std::thread([this] {
            while (!stop_.load()) {
                auto start = std::chrono::steady_clock::now();
                refresh_once();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                int sleep_ms = std::max(50, options_.poll_ms - static_cast<int>(elapsed));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        });
    }

    void stop() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    DashboardSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return snapshot_;
    }

    std::string health_json() const {
        auto s = snapshot();
        std::string out = "{";
        out += json_pair("version", blazerules::VERSION);
        out += ",";
        out += json_pair("last_update_ms", s.last_update_ms);
        out += ",\"sources\":[";
        bool first = true;
        for (const auto& [_, src] : s.sources) {
            if (!first) out += ",";
            first = false;
            out += source_json(src);
        }
        out += "]}";
        return out;
    }

    std::string summary_json() const {
        auto s = snapshot();
        double records = metric_value(s.metrics, "blazerules_records_evaluated_total");
        double batches = metric_value(s.metrics, "blazerules_batches_evaluated_total");
        double skipped = metric_value(s.metrics, "blazerules_records_skipped_total");
        double matched = metric_value(s.metrics, "blazerules_records_matched_total");
        if (records == 0.0 && s.decisions.rows_seen > 0) {
            records = static_cast<double>(s.decisions.rows_seen);
            matched = static_cast<double>(s.decisions.matched_seen);
        }
        double match_rate = records > 0.0 ? matched / records : 0.0;
        double total_ms = histogram_mean_us(s.metrics, "blazerules_batch_total_latency_us") / 1000.0;
        double eval_ms = histogram_mean_us(s.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
        double transpose_ms = histogram_mean_us(s.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;
        std::string active_version;
        if (!s.decisions.ruleset_versions.empty()) active_version = *s.decisions.ruleset_versions.rbegin();

        std::string out = "{";
        out += json_pair("version", blazerules::VERSION);
        out += ",";
        out += json_pair("last_update_ms", s.last_update_ms);
        out += ",";
        out += json_pair("active_ruleset_version", active_version);
        out += ",\"overview\":{";
        out += json_pair("records_evaluated", records);
        out += ",";
        out += json_pair("batches_evaluated", batches);
        out += ",";
        out += json_pair("records_matched", matched);
        out += ",";
        out += json_pair("records_skipped", skipped);
        out += ",";
        out += json_pair("match_rate", match_rate);
        out += ",";
        out += json_pair("recent_records_per_sec", s.recent_rps);
        out += ",";
        out += json_pair("recent_log_records", s.decisions.rows_seen);
        out += ",";
        out += json_pair("avg_score_recent", s.decisions.avg_score);
        out += "},\"timing_ms\":{";
        out += json_pair("total", total_ms);
        out += ",";
        out += json_pair("evaluation", eval_ms);
        out += ",";
        out += json_pair("transpose", transpose_ms);
        out += "},\"decision_counts\":";
        out += map_to_json(s.decisions.decision_counts);
        out += ",\"risk_band_counts\":";
        out += map_to_json(s.decisions.risk_band_counts);
        out += ",\"history\":[";
        for (size_t i = 0; i < s.history.size(); ++i) {
            if (i) out += ",";
            const auto& h = s.history[i];
            out += "{";
            out += json_pair("ts_ms", h.ts_ms);
            out += ",";
            out += json_pair("rps", h.rps);
            out += ",";
            out += json_pair("total_ms", h.total_ms);
            out += ",";
            out += json_pair("evaluation_ms", h.evaluation_ms);
            out += ",";
            out += json_pair("transpose_ms", h.transpose_ms);
            out += "}";
        }
        out += "]}";
        return out;
    }

    std::string metrics_json() const {
        auto s = snapshot();
        std::string out = "{\"last_success_ms\":";
        out += std::to_string(s.metrics.last_success_ms);
        out += ",\"last_error\":\"";
        out += json_escape(s.metrics.last_error);
        out += "\",\"metrics\":[";
        for (size_t i = 0; i < s.metrics.entries.size(); ++i) {
            if (i) out += ",";
            out += metric_entry_json(s.metrics.entries[i]);
        }
        out += "]}";
        return out;
    }

    std::string decisions_json(size_t limit) const {
        auto s = snapshot();
        std::string out = "{\"total_recent\":";
        out += std::to_string(s.decisions.rows_seen);
        out += ",\"rows\":[";
        size_t n = std::min(limit, s.decisions.recent.size());
        size_t start = s.decisions.recent.size() > n ? s.decisions.recent.size() - n : 0;
        for (size_t i = start; i < s.decisions.recent.size(); ++i) {
            if (i != start) out += ",";
            const auto& r = s.decisions.recent[i];
            out += "{";
            out += json_pair("ts_ms", r.ts_ms);
            out += ",";
            out += json_pair("ruleset_version", r.ruleset_version);
            out += ",";
            out += json_pair("batch_row", r.batch_row);
            out += ",";
            out += json_bool_pair("matched", r.matched);
            out += ",";
            out += json_pair("decision", r.decision);
            out += ",";
            out += json_pair("score", r.score);
            out += ",";
            out += json_pair("risk_band", r.risk_band);
            out += ",";
            out += json_pair("winning_rule_id", r.winning_rule_id);
            out += "}";
        }
        out += "]}";
        return out;
    }

    std::string rules_json(size_t limit) const {
        auto s = snapshot();
        struct RuleRow {
            std::string id;
            double fired = 0.0;
            double fire_rate = 0.0;
            int64_t winning_recent = 0;
        };
        std::map<std::string, RuleRow> rows;
        for (const auto& entry : s.metrics.entries) {
            auto it = entry.labels.find("rule_id");
            if (it == entry.labels.end()) continue;
            auto& row = rows[it->second];
            row.id = it->second;
            if (entry.name == "blazerules_rule_fired_total") row.fired = entry.value;
            if (entry.name == "blazerules_rule_fire_rate") row.fire_rate = entry.value;
        }
        for (const auto& [rule_id, count] : s.decisions.winning_rule_counts) {
            auto& row = rows[rule_id];
            row.id = rule_id;
            row.winning_recent = count;
        }
        std::vector<RuleRow> vec;
        for (auto& [_, row] : rows) vec.push_back(row);
        std::sort(vec.begin(), vec.end(), [](const RuleRow& a, const RuleRow& b) {
            if (a.fired != b.fired) return a.fired > b.fired;
            return a.winning_recent > b.winning_recent;
        });
        if (vec.size() > limit) vec.resize(limit);
        std::string out = "{\"rows\":[";
        for (size_t i = 0; i < vec.size(); ++i) {
            if (i) out += ",";
            out += "{";
            out += json_pair("rule_id", vec[i].id);
            out += ",";
            out += json_pair("fired_total", vec[i].fired);
            out += ",";
            out += json_pair("fire_rate", vec[i].fire_rate);
            out += ",";
            out += json_pair("winning_recent", vec[i].winning_recent);
            out += "}";
        }
        out += "]}";
        return out;
    }

    std::string errors_json(size_t limit) const {
        auto s = snapshot();
        std::string out = "{\"total_recent\":";
        out += std::to_string(s.errors.rows_seen);
        out += ",\"code_counts\":";
        out += map_to_json(s.errors.code_counts);
        out += ",\"rows\":[";
        size_t n = std::min(limit, s.errors.recent.size());
        size_t start = s.errors.recent.size() > n ? s.errors.recent.size() - n : 0;
        for (size_t i = start; i < s.errors.recent.size(); ++i) {
            if (i != start) out += ",";
            const auto& r = s.errors.recent[i];
            out += "{";
            out += json_pair("ts_ms", r.ts_ms);
            out += ",";
            out += json_pair("code", r.code);
            out += ",";
            out += json_pair("message", r.message);
            out += ",";
            out += json_pair("source", r.source);
            out += ",";
            out += json_pair("row_index", r.row_index);
            out += ",";
            out += json_pair("column_name", r.column_name);
            out += "}";
        }
        out += "]}";
        return out;
    }

    std::string benchmarks_json() const {
        auto s = snapshot();
        struct Agg {
            int64_t ok = 0;
            int64_t skipped = 0;
            int64_t failed = 0;
            std::vector<double> engine;
            std::vector<double> e2e;
            std::vector<double> gib;
        };
        std::map<std::string, Agg> aggs;
        for (const auto& r : s.benchmarks.rows) {
            auto& a = aggs[r.format];
            if (r.status == "ok") {
                a.ok += 1;
                a.engine.push_back(r.engine_rps);
                a.e2e.push_back(r.end_to_end_rps);
                a.gib.push_back(r.input_mib_per_sec / 1024.0);
            } else if (r.status == "skipped") {
                a.skipped += 1;
            } else {
                a.failed += 1;
            }
        }
        std::string out = "{\"last_success_ms\":";
        out += std::to_string(s.benchmarks.last_success_ms);
        out += ",\"last_error\":\"";
        out += json_escape(s.benchmarks.last_error);
        out += "\",\"formats\":[";
        bool first = true;
        for (auto& [fmt, agg] : aggs) {
            if (!first) out += ",";
            first = false;
            out += "{";
            out += json_pair("format", fmt);
            out += ",";
            out += json_pair("ok", agg.ok);
            out += ",";
            out += json_pair("skipped", agg.skipped);
            out += ",";
            out += json_pair("failed", agg.failed);
            out += ",";
            out += json_pair("median_engine_rps", median(agg.engine));
            out += ",";
            out += json_pair("median_end_to_end_rps", median(agg.e2e));
            out += ",";
            out += json_pair("median_engine_gib_sec", median(agg.gib));
            out += "}";
        }
        out += "],\"rows\":[";
        for (size_t i = 0; i < s.benchmarks.rows.size(); ++i) {
            if (i) out += ",";
            const auto& r = s.benchmarks.rows[i];
            out += "{";
            out += json_pair("format", r.format);
            out += ",";
            out += json_pair("status", r.status);
            out += ",";
            out += json_pair("error", r.error);
            out += ",";
            out += json_pair("target_json_bytes", r.target_json_bytes);
            out += ",";
            out += json_pair("actual_avg_json_bytes", r.actual_avg_json_bytes);
            out += ",";
            out += json_pair("records", r.records);
            out += ",";
            out += json_pair("batch_size", r.batch_size);
            out += ",";
            out += json_pair("matched_records", r.matched_records);
            out += ",";
            out += json_pair("rule_match_count_entries", r.rule_match_count_entries);
            out += ",";
            out += json_pair("engine_rps", r.engine_rps);
            out += ",";
            out += json_pair("end_to_end_rps", r.end_to_end_rps);
            out += ",";
            out += json_pair("input_gib_per_sec", r.input_mib_per_sec / 1024.0);
            out += ",";
            out += json_pair("avg_batch_latency_ms", r.avg_batch_latency_ms);
            out += ",";
            out += json_pair("p50_ms", r.p50_ms);
            out += ",";
            out += json_pair("p95_ms", r.p95_ms);
            out += ",";
            out += json_pair("p99_ms", r.p99_ms);
            out += "}";
        }
        out += "]}";
        return out;
    }

    std::string ruleset_json() const {
        auto s = snapshot();
        const auto& r = s.ruleset;
        std::string out = "{";
        out += json_pair("active_path", r.active_path);
        out += ",";
        out += json_pair("candidate_path", r.candidate_path);
        out += ",";
        out += json_pair("history_dir", r.history_dir);
        out += ",";
        out += json_bool_pair("active_configured", r.active_configured);
        out += ",";
        out += json_bool_pair("candidate_configured", r.candidate_configured);
        out += ",";
        out += json_bool_pair("active_valid", r.active_valid);
        out += ",";
        out += json_bool_pair("candidate_valid", r.candidate_valid);
        out += ",";
        out += json_pair("active_modified_ms", r.active_modified_ms);
        out += ",";
        out += json_pair("candidate_modified_ms", r.candidate_modified_ms);
        out += ",";
        out += json_pair("active_bytes", r.active_bytes);
        out += ",";
        out += json_pair("candidate_bytes", r.candidate_bytes);
        out += ",";
        out += json_pair("active_rule_count", r.active_rule_count);
        out += ",";
        out += json_pair("candidate_rule_count", r.candidate_rule_count);
        out += ",";
        out += json_pair("active_error", r.active_error);
        out += ",";
        out += json_pair("candidate_error", r.candidate_error);
        out += ",\"operator_counts\":";
        out += map_to_json(r.operator_counts);
        out += ",\"action_counts\":";
        out += map_to_json(r.action_counts);
        out += ",\"field_counts\":";
        out += map_to_json(r.field_counts);
        out += ",\"active_yaml\":\"";
        out += json_escape(r.active_yaml.size() > 200000 ? r.active_yaml.substr(0, 200000) : r.active_yaml);
        out += "\",\"candidate_yaml\":\"";
        out += json_escape(r.candidate_yaml.size() > 200000 ? r.candidate_yaml.substr(0, 200000) : r.candidate_yaml);
        out += "\",\"diff_rows\":[";
        for (size_t i = 0; i < r.diff_rows.size(); ++i) {
            if (i) out += ",";
            const auto& d = r.diff_rows[i];
            out += "{";
            out += json_pair("line", static_cast<int64_t>(d.line));
            out += ",";
            out += json_pair("kind", d.kind);
            out += ",";
            out += json_pair("active", d.active);
            out += ",";
            out += json_pair("candidate", d.candidate);
            out += "}";
        }
        out += "],\"versions\":[";
        for (size_t i = 0; i < r.versions.size(); ++i) {
            if (i) out += ",";
            const auto& v = r.versions[i];
            out += "{";
            out += json_pair("path", v.path);
            out += ",";
            out += json_pair("modified_ms", v.modified_ms);
            out += ",";
            out += json_pair("bytes", v.bytes);
            out += ",";
            out += json_pair("rule_count", v.rule_count);
            out += ",";
            out += json_bool_pair("valid", v.valid);
            out += ",";
            out += json_pair("error", v.error);
            out += "}";
        }
        out += "]}";
        return out;
    }

private:
    void refresh_once() {
        DashboardSnapshot next;
        next.last_update_ms = now_ms();

        SourceStatus decision_source;
        next.decisions = decision_tailer_.update(decision_source);
        next.sources[decision_source.name] = decision_source;

        SourceStatus dead_source;
        next.errors = dead_letter_tailer_.update(dead_source);
        next.sources[dead_source.name] = dead_source;

        SourceStatus metrics_source;
        next.metrics = metrics_scraper_.update(metrics_source);
        next.sources[metrics_source.name] = metrics_source;

        SourceStatus bench_source;
        next.benchmarks = benchmark_reader_.update(bench_source);
        next.sources[bench_source.name] = bench_source;

        SourceStatus rules_source;
        next.ruleset = ruleset_reader_.update(rules_source);
        next.sources[rules_source.name] = rules_source;

        double records = metric_value(next.metrics, "blazerules_records_evaluated_total");
        if (records > 0.0 && last_records_ms_ > 0 && next.last_update_ms > last_records_ms_ && records >= last_records_) {
            next.recent_rps = (records - last_records_) * 1000.0 /
                              static_cast<double>(next.last_update_ms - last_records_ms_);
        }
        if (records > 0.0) {
            last_records_ = records;
            last_records_ms_ = next.last_update_ms;
        }

        HistoryPoint hp;
        hp.ts_ms = next.last_update_ms;
        hp.rps = next.recent_rps;
        hp.total_ms = histogram_mean_us(next.metrics, "blazerules_batch_total_latency_us") / 1000.0;
        hp.evaluation_ms = histogram_mean_us(next.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
        hp.transpose_ms = histogram_mean_us(next.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;

        {
            std::lock_guard<std::mutex> lock(mu_);
            next.history = snapshot_.history;
            if (hp.rps > 0.0 || hp.total_ms > 0.0 || !next.metrics.entries.empty() ||
                next.decisions.rows_seen > 0 || !next.benchmarks.rows.empty()) {
                next.history.push_back(hp);
                if (next.history.size() > 180) {
                    next.history.erase(next.history.begin(), next.history.begin() + static_cast<long>(next.history.size() - 180));
                }
            }
            snapshot_ = std::move(next);
        }
    }

    Options options_;
    DecisionLogTailer decision_tailer_;
    DeadLetterTailer dead_letter_tailer_;
    PrometheusScraper metrics_scraper_;
    BenchmarkReader benchmark_reader_;
    RulesetReader ruleset_reader_;
    mutable std::mutex mu_;
    DashboardSnapshot snapshot_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    double last_records_ = 0.0;
    int64_t last_records_ms_ = 0;
};

const char* INDEX_HTML = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BlazeRules Console</title>
<style>
:root{--bg:#f3f4f7;--nav:#202433;--nav2:#303545;--panel:#fff;--line:#d9dee7;--text:#111827;--muted:#667085;--soft:#eef1f6;--accent:#2563eb;--accent2:#65a3ff;--bad:#b42318;--warn:#b54708;--ok:#067647}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:13px/1.42 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.app{display:grid;grid-template-columns:214px 1fr;min-height:100vh}
nav{background:var(--nav);color:#d6d9e1;padding:14px 10px;position:sticky;top:0;height:100vh}
.brand{padding:10px 10px 16px;border-bottom:1px solid rgba(255,255,255,.12);margin-bottom:10px}
.brand h1{font-size:16px;margin:0;color:#fff;font-weight:700}.brand .sub{font-size:11px;color:#aab1c4;margin-top:3px}
.navbtn{width:100%;border:0;background:transparent;color:#c9cedb;text-align:left;padding:8px 10px;border-radius:5px;cursor:pointer;font:inherit;margin:1px 0}
.navbtn:hover{background:rgba(255,255,255,.07)}.navbtn.active{background:#394050;color:#fff}
main{min-width:0}
header{height:50px;display:flex;align-items:center;justify-content:space-between;padding:0 16px;border-bottom:1px solid var(--line);background:#fff;position:sticky;top:0;z-index:2}
.search{height:32px;border:1px solid var(--line);border-radius:4px;padding:0 10px;min-width:360px;color:#344054;background:#fbfcfe}
.topmeta{display:flex;align-items:center;gap:10px;color:var(--muted);font-size:12px}
.status{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:999px;padding:3px 8px;background:#fff;color:#475467}
.dot{width:7px;height:7px;border-radius:50%;background:#98a2b3}.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}
.wrap{padding:14px;display:grid;gap:12px}.view{display:none}.view.active{display:grid;gap:12px}
.grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:10px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:6px;overflow:hidden;min-width:0}
.panel h2{font-size:11px;text-transform:uppercase;color:#475467;letter-spacing:.05em;margin:0;padding:8px 10px;border-bottom:1px solid var(--line);background:#fbfcfe}
.metric{padding:10px}.metric .label{color:var(--muted);font-size:11px}.metric .value{font-size:21px;font-weight:700;margin-top:3px;white-space:nowrap}.metric .hint{font-size:11px;color:var(--muted);margin-top:2px}
.span2{grid-column:span 2}.span3{grid-column:span 3}.span4{grid-column:span 4}.span5{grid-column:span 5}.span6{grid-column:span 6}.span7{grid-column:span 7}.span8{grid-column:span 8}.span12{grid-column:span 12}
table{width:100%;border-collapse:collapse}th,td{padding:7px 8px;border-bottom:1px solid #edf0f3;text-align:left;vertical-align:top;white-space:nowrap}th{font-size:11px;color:#475467;background:#fbfcfe;font-weight:650;position:sticky;top:0}td.mono,.mono,pre{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.scroll{max-height:360px;overflow:auto}.scroll.tall{max-height:620px}.empty{padding:18px;color:var(--muted)}.muted{color:var(--muted)}
.bars{padding:10px;display:grid;gap:8px}.barrow{display:grid;grid-template-columns:110px 1fr 80px;align-items:center;gap:8px}.bar{height:7px;background:var(--soft);border-radius:999px;overflow:hidden}.bar span{display:block;height:100%;background:var(--accent);width:0}.bar.warn span{background:var(--warn)}.bar.ok span{background:var(--ok)}
svg{width:100%;height:88px;display:block}.timeline{padding:10px}.timeline-bars{height:92px;display:flex;align-items:end;gap:2px;border-bottom:1px solid var(--line);padding-top:4px}.tick{flex:1;background:#9fc5df;min-height:1px;border-top:2px solid #ea6b5f}.pill{display:inline-flex;border:1px solid var(--line);border-radius:999px;padding:2px 7px;background:#fff;color:#475467}.pill.ok{color:var(--ok);border-color:#b7dfc9}.pill.bad{color:var(--bad);border-color:#f0b6b3}
pre{margin:0;padding:10px;background:#fbfcfe;white-space:pre;overflow:auto;max-height:620px;font-size:12px;line-height:1.45}.diff-add{background:#ecfdf3}.diff-del{background:#fff1f0}.diff-chg{background:#fffbeb}
.split{display:grid;grid-template-columns:1fr 1fr;gap:10px}.facets{display:grid;gap:8px;padding:10px}.facet{border-bottom:1px solid #edf0f3;padding-bottom:8px}.facet strong{display:block;margin-bottom:4px}
@media(max-width:1100px){.app{grid-template-columns:1fr}nav{position:static;height:auto}.navbtn{display:inline-block;width:auto}.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.span2,.span3,.span4,.span5,.span6,.span7,.span8,.span12{grid-column:span 2}.search{min-width:180px}}
</style>
</head>
<body>
<div class="app">
<nav>
  <div class="brand"><h1>BlazeRules</h1><div class="sub">local operations console</div></div>
  <button class="navbtn active" data-tab="overview">Overview</button>
  <button class="navbtn" data-tab="logs">Event Timeline</button>
  <button class="navbtn" data-tab="rules">Rule Fire Rates</button>
  <button class="navbtn" data-tab="ruleset">Ruleset Visualizer</button>
  <button class="navbtn" data-tab="errors">Errors / DLQ</button>
  <button class="navbtn" data-tab="benchmarks">Benchmarks</button>
  <button class="navbtn" data-tab="system">System</button>
</nav>
<main>
<header>
  <input id="filter" class="search" placeholder="Filter visible tables by decision, rule, error, or source">
  <div class="topmeta"><span id="topline">Loading...</span><span class="status"><span class="dot" id="healthdot"></span><span id="healthtext">starting</span></span></div>
</header>
<div class="wrap">
  <section id="overview" class="view active">
    <div class="grid" id="cards"></div>
    <div class="grid">
      <div class="panel span7"><h2>Event Timeline</h2><div class="timeline"><div id="timelineBars" class="timeline-bars"></div><svg id="spark"></svg></div></div>
      <div class="panel span5"><h2>Decision Distribution</h2><div class="scroll"><table><thead><tr><th>Decision</th><th>Recent Count</th><th>Share</th></tr></thead><tbody id="decisionDist"></tbody></table></div></div>
    </div>
    <div class="grid">
      <div class="panel span6"><h2>Latency Breakdown</h2><div class="bars" id="latencyBars"></div></div>
      <div class="panel span6"><h2>Top Winning Rules</h2><div class="scroll"><table><thead><tr><th>Rule</th><th>Winning Recent</th></tr></thead><tbody id="winningRows"></tbody></table></div></div>
    </div>
  </section>

  <section id="logs" class="view">
    <div class="grid">
      <div class="panel span3"><h2>Facets</h2><div class="facets" id="logFacets"></div></div>
      <div class="panel span9"><h2>Recent Decision Events</h2><div class="scroll tall"><table><thead><tr><th>Time</th><th>Matched</th><th>Decision</th><th>Score</th><th>Risk</th><th>Winning Rule</th><th>Ruleset</th></tr></thead><tbody id="decisionRows"></tbody></table></div></div>
    </div>
  </section>

  <section id="rules" class="view">
    <div class="grid">
      <div class="panel span8"><h2>Top Rules</h2><div class="scroll tall"><table><thead><tr><th>Rule</th><th>Fired Total</th><th>Fire Rate</th><th>Winning Recent</th></tr></thead><tbody id="ruleRows"></tbody></table></div></div>
      <div class="panel span4"><h2>Rule Plan Overview</h2><div class="bars" id="operatorBars"></div></div>
    </div>
  </section>

  <section id="ruleset" class="view">
    <div class="grid">
      <div class="panel span3"><h2>Validation</h2><div class="bars" id="rulesetSummary"></div></div>
      <div class="panel span3"><h2>Actions</h2><div class="scroll"><table><thead><tr><th>Action</th><th>Rules</th></tr></thead><tbody id="actionRows"></tbody></table></div></div>
      <div class="panel span6"><h2>Version History</h2><div class="scroll"><table><thead><tr><th>Modified</th><th>Rules</th><th>Valid</th><th>Path</th></tr></thead><tbody id="versionRows"></tbody></table></div></div>
    </div>
    <div class="grid">
      <div class="panel span6"><h2>Active YAML</h2><pre id="activeYaml"></pre></div>
      <div class="panel span6"><h2>Candidate Diff</h2><div class="scroll tall"><table><thead><tr><th>Line</th><th>Kind</th><th>Active</th><th>Candidate</th></tr></thead><tbody id="diffRows"></tbody></table></div></div>
    </div>
  </section>

  <section id="errors" class="view">
    <div class="grid"><div class="panel span3"><h2>Error Counts</h2><div class="scroll"><table><thead><tr><th>Code</th><th>Count</th></tr></thead><tbody id="errorCounts"></tbody></table></div></div><div class="panel span9"><h2>Recent Dead Letters</h2><div class="scroll tall"><table><thead><tr><th>Time</th><th>Code</th><th>Column</th><th>Message</th></tr></thead><tbody id="errorRows"></tbody></table></div></div></div>
  </section>

  <section id="benchmarks" class="view">
    <div class="grid"><div class="panel span3"><h2>By Format</h2><div class="scroll"><table><thead><tr><th>Format</th><th>OK</th><th>Median Engine</th><th>Median GiB/s</th></tr></thead><tbody id="benchFormats"></tbody></table></div></div><div class="panel span9"><h2>Matrix Rows</h2><div class="scroll tall"><table><thead><tr><th>Format</th><th>Size</th><th>Records</th><th>Batch</th><th>Status</th><th>Engine r/s</th><th>GiB/s</th><th>p95 ms</th></tr></thead><tbody id="benchRows"></tbody></table></div></div></div>
  </section>

  <section id="system" class="view">
    <div class="panel"><h2>Sources</h2><div class="scroll"><table><thead><tr><th>Name</th><th>Configured</th><th>Active</th><th>Location</th><th>Bytes</th><th>Last Error</th></tr></thead><tbody id="sourceRows"></tbody></table></div></div>
    <div class="panel"><h2>Raw Metrics</h2><div class="scroll tall"><table><thead><tr><th>Name</th><th>Value</th><th>Labels</th></tr></thead><tbody id="metricRows"></tbody></table></div></div>
  </section>
</div>
</main>
</div>
<script>
const $=id=>document.getElementById(id);
const fmt=n=>Number(n||0).toLocaleString(undefined,{maximumFractionDigits:0});
const f2=n=>Number(n||0).toLocaleString(undefined,{maximumFractionDigits:2});
const dt=ms=>ms?new Date(ms).toLocaleString():"";
const tm=ms=>ms?new Date(ms).toLocaleTimeString():"";
const esc=s=>String(s??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
function cell(v,cls=""){return `<td class="${cls}">${esc(v)}</td>`}
function rows(obj,total=0){let entries=Object.entries(obj||{}).sort((a,b)=>b[1]-a[1]);return entries.map(([k,v])=>`<tr>${cell(k,"mono")}${cell(fmt(v))}${cell(total?f2(v/total*100)+"%":"")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No data</td></tr>`}
function labels(o){return Object.entries(o||{}).map(([k,v])=>`${k}=${v}`).join(", ")}
function visibleText(row){return row.textContent.toLowerCase()}
function applyFilter(){let q=$("filter").value.trim().toLowerCase();document.querySelectorAll("tbody tr").forEach(r=>{r.style.display=!q||visibleText(r).includes(q)?"":"none"})}
function setTabs(){document.querySelectorAll(".navbtn").forEach(b=>b.onclick=()=>{document.querySelectorAll(".navbtn,.view").forEach(x=>x.classList.remove("active"));b.classList.add("active");$(b.dataset.tab).classList.add("active");applyFilter()});$("filter").oninput=applyFilter}
function spark(points){if(!points||!points.length)return "";let vals=points.map(p=>p.rps||p.total_ms||0),max=Math.max(...vals,1);return points.map((p,i)=>`${i/(points.length-1||1)*100},${88-(vals[i]/max)*78-5}`).join(" ")}
function tableBars(obj,limit=16){let entries=Object.entries(obj||{}).sort((a,b)=>b[1]-a[1]).slice(0,limit),mx=Math.max(...entries.map(x=>x[1]),1);return entries.map(([k,v])=>`<div class="barrow"><span class="mono" title="${esc(k)}">${esc(k).slice(0,26)}</span><div class="bar"><span style="width:${Math.min(100,v/mx*100)}%"></span></div><span class="mono">${fmt(v)}</span></div>`).join("")||`<div class="empty">No data</div>`}
function timeline(points){if(!points||!points.length)return "";let vals=points.map(p=>p.rps||0),mx=Math.max(...vals,1);return vals.map(v=>`<div class="tick" style="height:${Math.max(2,v/mx*88)}px" title="${fmt(v)} r/s"></div>`).join("")}
async function j(path){let r=await fetch(path,{cache:"no-store"});return r.json()}
async function refresh(){
 try{
  const [s,h,d,r,e,b,m,rs]=await Promise.all([j("/api/summary"),j("/api/health"),j("/api/decisions?limit=800"),j("/api/rules?limit=200"),j("/api/errors?limit=300"),j("/api/benchmarks"),j("/api/metrics"),j("/api/ruleset")]);
  $("topline").textContent=`version ${s.version||""} | ruleset ${s.active_ruleset_version||"unknown"} | updated ${tm(s.last_update_ms)}`;
  let active=(h.sources||[]).some(x=>x.active); $("healthdot").className="dot "+(active?"ok":"bad"); $("healthtext").textContent=active?"observing":"no active sources";
  let o=s.overview||{}, t=s.timing_ms||{};
  const cards=[["Records",fmt(o.records_evaluated)],["Matched",fmt(o.records_matched),f2((o.match_rate||0)*100)+"% match"],["Skipped",fmt(o.records_skipped)],["Batches",fmt(o.batches_evaluated)],["Recent r/s",fmt(o.recent_records_per_sec)],["Avg score",f2(o.avg_score_recent)]];
  $("cards").innerHTML=cards.map(c=>`<div class="panel metric span2"><div class="label">${c[0]}</div><div class="value">${c[1]}</div><div class="hint">${c[2]||""}</div></div>`).join("");
  const lat=[["total",t.total],["evaluation",t.evaluation],["transpose",t.transpose]], mx=Math.max(...lat.map(x=>x[1]||0),1);
  $("latencyBars").innerHTML=lat.map(x=>`<div class="barrow"><span>${x[0]}</span><div class="bar"><span style="width:${Math.min(100,(x[1]||0)/mx*100)}%"></span></div><span class="mono">${f2(x[1])} ms</span></div>`).join("");
  $("spark").innerHTML=`<polyline fill="none" stroke="#2563eb" stroke-width="2" points="${spark(s.history)}"></polyline>`;
  $("timelineBars").innerHTML=timeline(s.history);
  let decisionTotal=Object.values(s.decision_counts||{}).reduce((a,b)=>a+b,0);
  $("decisionDist").innerHTML=rows(s.decision_counts,decisionTotal);
  $("winningRows").innerHTML=Object.entries((d.rows||[]).reduce((a,x)=>{if(x.winning_rule_id)a[x.winning_rule_id]=(a[x.winning_rule_id]||0)+1;return a},{})).sort((a,b)=>b[1]-a[1]).slice(0,20).map(([k,v])=>`<tr>${cell(k,"mono")}${cell(fmt(v))}</tr>`).join("")||`<tr><td colspan="2" class="empty">No winning rules observed</td></tr>`;
  $("decisionRows").innerHTML=(d.rows||[]).reverse().map(x=>`<tr>${cell(dt(x.ts_ms))}${cell(x.matched?"yes":"no")}${cell(x.decision,"mono")}${cell(f2(x.score))}${cell(x.risk_band,"mono")}${cell(x.winning_rule_id,"mono")}${cell(x.ruleset_version,"mono")}</tr>`).join("")||`<tr><td colspan="7" class="empty">No decision log rows</td></tr>`;
  $("logFacets").innerHTML=`<div class="facet"><strong>Decision</strong><table><tbody>${rows(s.decision_counts)}</tbody></table></div><div class="facet"><strong>Risk Band</strong><table><tbody>${rows(s.risk_band_counts)}</tbody></table></div>`;
  $("ruleRows").innerHTML=(r.rows||[]).map(x=>`<tr>${cell(x.rule_id,"mono")}${cell(fmt(x.fired_total))}${cell(f2((x.fire_rate||0)*100)+"%")}${cell(fmt(x.winning_recent))}</tr>`).join("")||`<tr><td colspan="4" class="empty">No rule metrics yet</td></tr>`;
  $("operatorBars").innerHTML=tableBars(rs.operator_counts,22);
  $("rulesetSummary").innerHTML=`<div class="barrow"><span>active</span><span class="pill ${rs.active_valid?"ok":"bad"}">${rs.active_valid?"valid":"invalid"}</span><span>${fmt(rs.active_rule_count)} rules</span></div><div class="barrow"><span>candidate</span><span class="pill ${rs.candidate_valid?"ok":(rs.candidate_configured?"bad":"")}">${rs.candidate_configured?(rs.candidate_valid?"valid":"invalid"):"none"}</span><span>${fmt(rs.candidate_rule_count)} rules</span></div><div class="muted mono">${esc(rs.active_error||rs.candidate_error||rs.active_path||"No rules path configured")}</div>`;
  $("actionRows").innerHTML=rows(rs.action_counts);
  $("versionRows").innerHTML=(rs.versions||[]).map(x=>`<tr>${cell(dt(x.modified_ms))}${cell(fmt(x.rule_count))}${cell(x.valid?"yes":"no")}${cell(x.path,"mono")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No version directory configured</td></tr>`;
  $("activeYaml").textContent=rs.active_yaml||"No active rules YAML configured. Start with --rules PATH.";
  $("diffRows").innerHTML=(rs.diff_rows||[]).map(x=>`<tr class="${x.kind==="added"?"diff-add":(x.kind==="removed"?"diff-del":"diff-chg")}">${cell(x.line)}${cell(x.kind)}${cell(x.active,"mono")}${cell(x.candidate,"mono")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No candidate diff. Start with --candidate-rules PATH.</td></tr>`;
  $("errorCounts").innerHTML=rows(e.code_counts);
  $("errorRows").innerHTML=(e.rows||[]).reverse().map(x=>`<tr>${cell(dt(x.ts_ms))}${cell(x.code,"mono")}${cell(x.column_name,"mono")}${cell(x.message)}</tr>`).join("")||`<tr><td colspan="4" class="empty">No dead-letter rows</td></tr>`;
  $("benchFormats").innerHTML=(b.formats||[]).map(x=>`<tr>${cell(x.format,"mono")}${cell(fmt(x.ok))}${cell(fmt(x.median_engine_rps))}${cell(f2(x.median_engine_gib_sec))}</tr>`).join("")||`<tr><td colspan="4" class="empty">No benchmark data</td></tr>`;
  $("benchRows").innerHTML=(b.rows||[]).slice(0,900).map(x=>`<tr>${cell(x.format,"mono")}${cell(fmt(x.actual_avg_json_bytes)+" B")}${cell(fmt(x.records))}${cell(fmt(x.batch_size))}${cell(x.status,"mono")}${cell(fmt(x.engine_rps))}${cell(f2(x.input_gib_per_sec))}${cell(f2(x.p95_ms))}</tr>`).join("");
  $("sourceRows").innerHTML=(h.sources||[]).map(x=>`<tr>${cell(x.name,"mono")}${cell(x.configured?"yes":"no")}${cell(x.active?"yes":"no")}${cell(x.location,"mono")}${cell(fmt(x.bytes))}${cell(x.last_error)}</tr>`).join("");
  $("metricRows").innerHTML=(m.metrics||[]).slice(0,1500).map(x=>`<tr>${cell(x.name,"mono")}${cell(f2(x.value))}${cell(labels(x.labels),"mono")}</tr>`).join("")||`<tr><td colspan="3" class="empty">No metrics endpoint configured or reachable</td></tr>`;
  applyFilter();
 }catch(err){$("healthdot").className="dot bad";$("healthtext").textContent="poll failed";console.error(err)}
}
setTabs(); refresh(); setInterval(refresh,1000);
</script>
</body>
</html>
)HTML";

int limit_from_request(const httplib::Request& req, const char* name, int fallback, int max_value) {
    if (!req.has_param(name)) return fallback;
    int v = std::atoi(req.get_param_value(name).c_str());
    if (v <= 0) return fallback;
    return std::min(v, max_value);
}

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") opt.host = need("--host");
        else if (a == "--port") opt.port = std::atoi(need("--port").c_str());
        else if (a == "--poll-ms") opt.poll_ms = std::atoi(need("--poll-ms").c_str());
        else if (a == "--tail-lines") opt.tail_lines = static_cast<size_t>(std::max(1, std::atoi(need("--tail-lines").c_str())));
        else if (a == "--decision-log") opt.decision_log = need("--decision-log");
        else if (a == "--dead-letter-log") opt.dead_letter_log = need("--dead-letter-log");
        else if (a == "--metrics-url") opt.metrics_url = need("--metrics-url");
        else if (a == "--results-jsonl") opt.results_jsonl = need("--results-jsonl");
        else if (a == "--rules") opt.rules_path = need("--rules");
        else if (a == "--candidate-rules") opt.candidate_rules_path = need("--candidate-rules");
        else if (a == "--rules-history-dir") opt.rules_history_dir = need("--rules-history-dir");
        else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: blazerules_dashboard [options]\n\n"
                << "Options:\n"
                << "  --host HOST                 default 127.0.0.1\n"
                << "  --port PORT                 default 9470\n"
                << "  --poll-ms MS                default 1000\n"
                << "  --tail-lines N              default 5000\n"
                << "  --decision-log PATH         compact decision NDJSON\n"
                << "  --dead-letter-log PATH      compact dead-letter NDJSON\n"
                << "  --metrics-url URL           Prometheus URL, e.g. http://127.0.0.1:9464/metrics\n"
                << "  --results-jsonl PATH        stress_matrix JSONL\n"
                << "  --rules PATH                active rules YAML for visualizer\n"
                << "  --candidate-rules PATH      candidate rules YAML for validation/diff\n"
                << "  --rules-history-dir DIR     directory of YAML versions\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            std::exit(2);
        }
    }
    if (opt.port <= 0) opt.port = 9470;
    if (opt.poll_ms <= 0) opt.poll_ms = 1000;
    return opt;
}

} // namespace

int main(int argc, char** argv) {
    Options options = parse_args(argc, argv);
    if (options.host == "0.0.0.0") {
        std::cerr << "warning: dashboard has no authentication and is bound to 0.0.0.0\n";
    }

    DashboardServer dashboard(options);
    dashboard.start();

    httplib::Server server;
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(INDEX_HTML, "text/html; charset=utf-8");
    });
    server.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    server.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.health_json(), "application/json");
    });
    server.Get("/api/summary", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.summary_json(), "application/json");
    });
    server.Get("/api/metrics", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.metrics_json(), "application/json");
    });
    server.Get("/api/decisions", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.decisions_json(static_cast<size_t>(limit_from_request(req, "limit", 500, 5000))),
                        "application/json");
    });
    server.Get("/api/rules", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.rules_json(static_cast<size_t>(limit_from_request(req, "limit", 100, 5000))),
                        "application/json");
    });
    server.Get("/api/errors", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(dashboard.errors_json(static_cast<size_t>(limit_from_request(req, "limit", 200, 5000))),
                        "application/json");
    });
    server.Get("/api/benchmarks", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.benchmarks_json(), "application/json");
    });
    server.Get("/api/ruleset", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(dashboard.ruleset_json(), "application/json");
    });

    std::cout << "BlazeRules dashboard listening on http://" << options.host << ":" << options.port << "\n";
    std::cout << "Sources: decision_log=" << (options.decision_log.empty() ? "<none>" : options.decision_log)
              << " dead_letter_log=" << (options.dead_letter_log.empty() ? "<none>" : options.dead_letter_log)
              << " metrics_url=" << (options.metrics_url.empty() ? "<none>" : options.metrics_url)
              << " results_jsonl=" << (options.results_jsonl.empty() ? "<none>" : options.results_jsonl)
              << " rules=" << (options.rules_path.empty() ? "<none>" : options.rules_path)
              << "\n";

    bool ok = server.listen(options.host, options.port);
    dashboard.stop();
    if (!ok) {
        std::cerr << "failed to bind " << options.host << ":" << options.port << "\n";
        return 1;
    }
    return 0;
}
