#include "dashboard_server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <utility>

#include "blazerules/version.h"
#include "json_util.h"

namespace fs = std::filesystem;

DashboardServer::DashboardServer(Options options)
    : options_(std::move(options)),
      decision_tailer_(options_.decision_log, options_.tail_lines, options_.max_index_rows),
      dead_letter_tailer_(options_.dead_letter_log, options_.tail_lines),
      metrics_scraper_(options_.metrics_url),
      benchmark_reader_(options_.results_jsonl),
      ruleset_reader_(options_.rules_path, options_.candidate_rules_path, options_.rules_history_dir) {}

DashboardServer::~DashboardServer() {
    stop();
}

void DashboardServer::start() {
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

void DashboardServer::stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}

DashboardSnapshot DashboardServer::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
}

std::string DashboardServer::health_json() const {
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

std::string DashboardServer::summary_json() const {
    auto s = snapshot();
    double records = metric_value(s.metrics, "blazerules_records_evaluated_total");
    double batches = metric_value(s.metrics, "blazerules_batches_evaluated_total");
    double skipped = metric_value(s.metrics, "blazerules_records_skipped_total");
    double matched = metric_value(s.metrics, "blazerules_records_matched_total");
    if (records == 0.0 && s.decisions.rows_seen > 0) {
        records = static_cast<double>(s.decisions.rows_seen);
        matched = static_cast<double>(s.decisions.matched_seen);
    }
    if (skipped == 0.0 && s.errors.rows_seen > 0) {
        skipped = static_cast<double>(s.errors.rows_seen);
    }
    double match_rate = records > 0.0 ? matched / records : 0.0;
    double total_ms = histogram_mean_us(s.metrics, "blazerules_batch_total_latency_us") / 1000.0;
    double eval_ms = histogram_mean_us(s.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
    double transpose_ms = histogram_mean_us(s.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;
    uintmax_t decision_log_bytes = 0;
    auto decision_source = s.sources.find("decision_log");
    if (decision_source != s.sources.end()) decision_log_bytes = decision_source->second.bytes;
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
    out += json_pair("recent_bytes_per_sec", s.recent_bytes_per_sec);
    out += ",";
    out += json_pair("recent_log_records", s.decisions.rows_seen);
    out += ",";
    out += json_pair("decision_log_bytes", static_cast<double>(decision_log_bytes));
    out += ",";
    out += json_bool_pair("has_metrics", !s.metrics.entries.empty());
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
        out += json_pair("bytes_per_sec", h.bytes_per_sec);
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

std::string DashboardServer::metrics_json() const {
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

namespace {

std::string facet_json(const std::map<std::string, int64_t>& facets) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : facets) {
        if (!first) out += ",";
        first = false;
        out += json_pair(key, value);
    }
    out += "}";
    return out;
}

std::string decisions_payload(const DecisionQueryResult& result, const DecisionQuery& query) {
    size_t limit = std::max<size_t>(1, query.limit);
    size_t offset = query.offset;
    std::string out = "{\"total_recent\":";
    out += std::to_string(result.total_matches);
    out += ",\"indexed_rows\":";
    out += std::to_string(result.indexed_rows);
    out += ",\"truncated\":";
    out += result.truncated ? "true" : "false";
    out += ",\"offset\":";
    out += std::to_string(offset);
    out += ",\"limit\":";
    out += std::to_string(limit);
    out += ",\"has_more\":";
    out += (offset + result.rows.size() < static_cast<size_t>(std::max<int64_t>(result.total_matches, 0))) ? "true" : "false";
    out += ",\"decision_facets\":";
    out += facet_json(result.decision_facets);
    out += ",\"risk_band_facets\":";
    out += facet_json(result.risk_band_facets);
    out += ",\"rows\":[";
    for (size_t i = 0; i < result.rows.size(); ++i) {
        if (i) out += ",";
        const auto& r = result.rows[i];
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

}  // namespace

std::string DashboardServer::decisions_json(const DecisionQuery& query) const {
    DecisionQueryResult result = decision_tailer_.query(query);
    return decisions_payload(result, query);
}

std::string DashboardServer::rules_json(size_t limit) const {
    auto s = snapshot();
    struct RuleRow {
        std::string id;
        double fired = 0.0;
        double fire_rate = 0.0;
        int64_t winning_total = 0;
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
        row.winning_total = count;
        if (row.fired == 0.0) row.fired = static_cast<double>(count);
    }
    double denominator = s.decisions.rows_seen > 0
        ? static_cast<double>(s.decisions.rows_seen)
        : metric_value(s.metrics, "blazerules_records_evaluated_total");
    std::vector<RuleRow> vec;
    for (auto& [_, row] : rows) {
        if (row.fire_rate == 0.0 && denominator > 0.0 && row.fired > 0.0) {
            row.fire_rate = row.fired / denominator;
        }
        vec.push_back(row);
    }
    std::sort(vec.begin(), vec.end(), [](const RuleRow& a, const RuleRow& b) {
        if (a.fired != b.fired) return a.fired > b.fired;
        return a.winning_total > b.winning_total;
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
        out += json_pair("winning_total", vec[i].winning_total);
        out += "}";
    }
    out += "]}";
    return out;
}

std::string DashboardServer::errors_json(size_t limit) const {
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

std::string DashboardServer::benchmarks_json() const {
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

std::string DashboardServer::ruleset_json() const {
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

void DashboardServer::refresh_once() {
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
    if (records == 0.0) records = static_cast<double>(next.decisions.rows_seen);
    uintmax_t decision_log_bytes = 0;
    auto decision_it = next.sources.find("decision_log");
    if (decision_it != next.sources.end()) decision_log_bytes = decision_it->second.bytes;
    if (records > 0.0 && last_records_ms_ > 0 && next.last_update_ms > last_records_ms_ && records >= last_records_) {
        next.recent_rps = (records - last_records_) * 1000.0 /
                          static_cast<double>(next.last_update_ms - last_records_ms_);
    }
    if (last_decision_log_bytes_ms_ > 0 && next.last_update_ms > last_decision_log_bytes_ms_ &&
        decision_log_bytes >= last_decision_log_bytes_) {
        next.recent_bytes_per_sec = static_cast<double>(decision_log_bytes - last_decision_log_bytes_) * 1000.0 /
                                    static_cast<double>(next.last_update_ms - last_decision_log_bytes_ms_);
    }
    if (records > 0.0) {
        last_records_ = records;
        last_records_ms_ = next.last_update_ms;
    }
    last_decision_log_bytes_ = decision_log_bytes;
    last_decision_log_bytes_ms_ = next.last_update_ms;

    HistoryPoint hp;
    hp.ts_ms = next.last_update_ms;
    hp.rps = next.recent_rps;
    hp.bytes_per_sec = next.recent_bytes_per_sec;
    hp.total_ms = histogram_mean_us(next.metrics, "blazerules_batch_total_latency_us") / 1000.0;
    hp.evaluation_ms = histogram_mean_us(next.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
    hp.transpose_ms = histogram_mean_us(next.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;

    std::lock_guard<std::mutex> lock(mu_);
    next.history = snapshot_.history;
    if (hp.rps > 0.0 || hp.bytes_per_sec > 0.0 || hp.total_ms > 0.0 || !next.metrics.entries.empty() || !next.benchmarks.rows.empty()) {
        next.history.push_back(hp);
        if (next.history.size() > 180) {
            next.history.erase(next.history.begin(), next.history.begin() + static_cast<long>(next.history.size() - 180));
        }
    }
    snapshot_ = std::move(next);
}
