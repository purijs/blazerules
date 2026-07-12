#include "dashboard_server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <utility>

#include "blazerules/resource_resolver.h"
#include "blazerules/version.h"
#include "json_util.h"

namespace fs = std::filesystem;

namespace {

std::string decision_local_root(const Options& o) {
    if (!o.decision_log_dir.empty()) {
        return blazerules::is_s3_uri(o.decision_log_dir)
            ? blazerules::s3_local_cache_dir(o.decision_log_dir)
            : o.decision_log_dir;
    }
    if (!o.decision_log.empty()) {
        if (!blazerules::is_s3_uri(o.decision_log)) return o.decision_log;
        std::string base = o.decision_log.substr(o.decision_log.find_last_of('/') + 1);
        if (base.empty()) base = "decision.log";
        return blazerules::s3_local_cache_dir(o.decision_log) + "/" + base;
    }
    return "";
}

std::string stats_instance_name(const fs::path& path) {
    fs::path p = path;
    if (p.extension() == ".stats") p = p.stem();
    return p.stem().string();
}

struct InputStats {
    double bytes = 0.0;
    double records = 0.0;
};

constexpr int64_t kTimelineSecondMs = 1000;
constexpr int64_t kTimelineMinuteMs = 60 * 1000;
constexpr int64_t kTimelineHourMs = 60 * 60 * 1000;
constexpr size_t kTimelineSecondPoints = 3600;
constexpr size_t kTimelineMinutePoints = 3 * 24 * 60;
constexpr size_t kTimelineHourPoints = 45 * 24;

int64_t bucket_start_ms(int64_t ts_ms, int64_t bucket_ms) {
    if (bucket_ms <= 0) return ts_ms;
    return (ts_ms / bucket_ms) * bucket_ms;
}

void trim_points(std::vector<HistoryPoint>& points, size_t max_points) {
    if (points.size() <= max_points) return;
    points.erase(points.begin(), points.begin() + static_cast<long>(points.size() - max_points));
}

void append_bucket(std::vector<HistoryPoint>& points, HistoryPoint p, int64_t bucket_ms, size_t max_points) {
    p.ts_ms = bucket_start_ms(p.ts_ms, bucket_ms);
    p.samples = std::max<int64_t>(1, p.samples);
    if (!points.empty() && points.back().ts_ms == p.ts_ms) {
        auto& last = points.back();
        const double existing = static_cast<double>(std::max<int64_t>(1, last.samples));
        const double incoming = static_cast<double>(p.samples);
        const double denom = existing + incoming;
        last.rps = (last.rps * existing + p.rps * incoming) / denom;
        last.bytes_per_sec = (last.bytes_per_sec * existing + p.bytes_per_sec * incoming) / denom;
        last.total_ms = (last.total_ms * existing + p.total_ms * incoming) / denom;
        last.evaluation_ms = (last.evaluation_ms * existing + p.evaluation_ms * incoming) / denom;
        last.transpose_ms = (last.transpose_ms * existing + p.transpose_ms * incoming) / denom;
        last.samples += p.samples;
    } else {
        points.push_back(p);
    }
    trim_points(points, max_points);
}

void append_timeline_buckets(std::vector<HistoryPoint>& seconds,
                             std::vector<HistoryPoint>& minutes,
                             std::vector<HistoryPoint>& hours,
                             const HistoryPoint& point) {
    append_bucket(seconds, point, kTimelineSecondMs, kTimelineSecondPoints);
    append_bucket(minutes, point, kTimelineMinuteMs, kTimelineMinutePoints);
    append_bucket(hours, point, kTimelineHourMs, kTimelineHourPoints);
}

int64_t choose_bucket_ms(int64_t from_ms, int64_t to_ms) {
    const int64_t span = std::max<int64_t>(0, to_ms - from_ms);
    if (span <= 60 * 60 * 1000) return kTimelineSecondMs;
    if (span <= 3LL * 24 * 60 * 60 * 1000) return kTimelineMinuteMs;
    return kTimelineHourMs;
}

const std::vector<HistoryPoint>& timeline_series(const DashboardSnapshot& s,
                                                 const std::string& instance,
                                                 int64_t bucket_ms) {
    if (!instance.empty()) {
        if (bucket_ms == kTimelineSecondMs) {
            auto it = s.timeline_1s_by_instance.find(instance);
            if (it != s.timeline_1s_by_instance.end()) return it->second;
        } else if (bucket_ms == kTimelineMinuteMs) {
            auto it = s.timeline_1m_by_instance.find(instance);
            if (it != s.timeline_1m_by_instance.end()) return it->second;
        } else {
            auto it = s.timeline_1h_by_instance.find(instance);
            if (it != s.timeline_1h_by_instance.end()) return it->second;
        }
    }
    if (bucket_ms == kTimelineSecondMs) return s.timeline_1s;
    if (bucket_ms == kTimelineMinuteMs) return s.timeline_1m;
    return s.timeline_1h;
}

InputStats parse_input_stats_line(const std::string& line) {
    InputStats stats;
    stats.bytes = json_number(line, "input_bytes").value_or(0.0);
    stats.records = json_number(line, "input_records").value_or(0.0);
    return stats;
}

std::map<std::string, InputStats> read_input_stats_by_instance(const Options& o) {
    std::map<std::string, InputStats> out;
    const std::string root = decision_local_root(o);
    if (root.empty()) return out;
    std::error_code ec;
    if (!o.decision_log_dir.empty()) {
        if (!fs::is_directory(root, ec)) return out;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".stats") continue;
            std::ifstream in(entry.path());
            std::string line;
            if (in && std::getline(in, line)) {
                const auto stats = parse_input_stats_line(line);
                auto& dst = out[stats_instance_name(entry.path())];
                dst.bytes += stats.bytes;
                dst.records += stats.records;
            }
        }
        return out;
    }
    if (!o.decision_log.empty()) {
        std::ifstream in(root + ".stats");
        std::string line;
        if (in && std::getline(in, line)) {
            out[stats_instance_name(fs::path(root + ".stats"))] = parse_input_stats_line(line);
        }
    }
    return out;
}

// Dead-letter source: an explicit --dead-letter-log wins; otherwise auto-read the
// dlq files from the decision-log directory (its local cache when that dir is s3).
std::string dead_letter_root(const Options& o) {
    if (!o.dead_letter_log.empty()) return o.dead_letter_log;
    if (!o.decision_log_dir.empty()) return decision_local_root(o);
    return "";
}

bool dead_letter_is_dir(const Options& o) {
    return o.dead_letter_log.empty() && !o.decision_log_dir.empty();
}

ErrorState scoped_errors(const ErrorState& errors, const std::string& instance) {
    if (instance.empty()) return errors;
    ErrorState scoped;
    auto count_it = errors.instance_counts.find(instance);
    if (count_it != errors.instance_counts.end()) scoped.rows_seen = count_it->second;
    if (scoped.rows_seen > 0) scoped.instance_counts[instance] = scoped.rows_seen;
    auto code_it = errors.code_counts_by_instance.find(instance);
    if (code_it != errors.code_counts_by_instance.end()) {
        scoped.code_counts = code_it->second;
        scoped.code_counts_by_instance[instance] = code_it->second;
    }
    for (const auto& row : errors.recent) {
        if (row.instance == instance) scoped.recent.push_back(row);
    }
    if (scoped.rows_seen == 0 && !scoped.recent.empty()) {
        scoped.rows_seen = static_cast<int64_t>(scoped.recent.size());
        scoped.instance_counts[instance] = scoped.rows_seen;
        for (const auto& row : scoped.recent) {
            if (!row.code.empty()) scoped.code_counts[row.code] += 1;
        }
        scoped.code_counts_by_instance[instance] = scoped.code_counts;
    }
    return scoped;
}

std::string rules_dir_local(const Options& o) {
    if (o.rules_dir.empty()) return "";
    return blazerules::is_s3_uri(o.rules_dir) ? blazerules::s3_local_cache_dir(o.rules_dir) : o.rules_dir;
}

}  // namespace

DashboardServer::DashboardServer(Options options)
    : options_(std::move(options)),
      decision_tailer_(decision_local_root(options_),
                       !options_.decision_log_dir.empty(),
                       options_.tail_lines, options_.max_index_rows),
      dead_letter_tailer_(dead_letter_root(options_), dead_letter_is_dir(options_), options_.tail_lines),
      metrics_scraper_(options_.metrics_url),
      benchmark_reader_(options_.results_jsonl),
      ruleset_reader_(options_.rules_path, rules_dir_local(options_),
                      options_.candidate_rules_path, options_.rules_history_dir) {
    if (!options_.decision_log_dir.empty() && blazerules::is_s3_uri(options_.decision_log_dir)) {
        s3_source_ = options_.decision_log_dir;
        s3_source_is_dir_ = true;
        s3_local_root_ = decision_local_root(options_);
    } else if (!options_.decision_log.empty() && blazerules::is_s3_uri(options_.decision_log)) {
        s3_source_ = options_.decision_log;
        s3_source_is_dir_ = false;
        s3_local_root_ = decision_local_root(options_);
    }
    if (!options_.rules_dir.empty() && blazerules::is_s3_uri(options_.rules_dir)) {
        s3_rules_source_ = options_.rules_dir;
        s3_rules_local_ = rules_dir_local(options_);
    }
}

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

std::string DashboardServer::summary_json(const std::string& instance) const {
    auto s = snapshot();
    const bool scoped = !instance.empty();
    DecisionState scoped_state;
    if (scoped) scoped_state = decision_tailer_.scoped_state(instance);
    const DecisionState& dec = scoped ? scoped_state : s.decisions;
    const ErrorState err = scoped ? scoped_errors(s.errors, instance) : s.errors;
    double records = 0.0;
    double batches = 0.0;
    double skipped = 0.0;
    double matched = 0.0;
    if (scoped) {
        records = static_cast<double>(dec.rows_seen);
        matched = static_cast<double>(dec.matched_seen);
        skipped = static_cast<double>(err.rows_seen);
        auto input_records = s.input_records_total_by_instance.find(instance);
        if (input_records != s.input_records_total_by_instance.end() && input_records->second > 0.0) {
            records = input_records->second;
        }
    } else {
        records = metric_value(s.metrics, "blazerules_records_evaluated_total");
        batches = metric_value(s.metrics, "blazerules_batches_evaluated_total");
        skipped = metric_value(s.metrics, "blazerules_records_skipped_total");
        matched = metric_value(s.metrics, "blazerules_records_matched_total");
        if (s.input_records_total > 0.0) {
            records = s.input_records_total;
        }
        if (records == 0.0 && s.decisions.rows_seen > 0) {
            records = static_cast<double>(s.decisions.rows_seen);
            matched = static_cast<double>(s.decisions.matched_seen);
        }
        if (skipped == 0.0 && err.rows_seen > 0) {
            skipped = static_cast<double>(err.rows_seen);
        }
    }
    double match_rate = records > 0.0 ? matched / records : 0.0;
    double total_ms = histogram_mean_us(s.metrics, "blazerules_batch_total_latency_us") / 1000.0;
    double eval_ms = histogram_mean_us(s.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
    double transpose_ms = histogram_mean_us(s.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;
    uintmax_t decision_log_bytes = 0;
    if (scoped) {
        auto lb = s.decisions.instance_log_bytes.find(instance);
        if (lb != s.decisions.instance_log_bytes.end()) decision_log_bytes = static_cast<uintmax_t>(lb->second);
    } else {
        auto decision_source = s.sources.find("decision_log");
        if (decision_source != s.sources.end()) decision_log_bytes = decision_source->second.bytes;
    }
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
    double recent_rps = s.recent_rps;
    double input_bps = s.recent_input_bytes_per_sec;
    if (scoped) {
        auto ir = s.recent_input_records_per_sec_by_instance.find(instance);
        recent_rps = ir == s.recent_input_records_per_sec_by_instance.end() ? 0.0 : ir->second;
        auto ib = s.recent_input_bytes_per_sec_by_instance.find(instance);
        input_bps = ib == s.recent_input_bytes_per_sec_by_instance.end() ? 0.0 : ib->second;
    }
    out += json_pair("recent_records_per_sec", recent_rps);
    out += ",";
    out += json_pair("recent_bytes_per_sec", s.recent_bytes_per_sec);
    out += ",";
    out += json_pair("recent_input_bytes_per_sec", input_bps);
    out += ",";
    out += json_pair("recent_log_records", dec.rows_seen);
    out += ",";
    out += json_pair("decision_log_bytes", static_cast<double>(decision_log_bytes));
    out += ",";
    out += json_bool_pair("has_metrics", !s.metrics.entries.empty());
    out += ",";
    out += json_pair("avg_score_recent", dec.avg_score);
    out += "},\"timing_ms\":{";
    out += json_pair("total", total_ms);
    out += ",";
    out += json_pair("evaluation", eval_ms);
    out += ",";
    out += json_pair("transpose", transpose_ms);
    out += "},\"decision_counts\":";
    out += map_to_json(dec.decision_counts);
    out += ",\"risk_band_counts\":";
    out += map_to_json(dec.risk_band_counts);
    out += ",\"instance_counts\":";
    out += map_to_json(s.decisions.instance_counts);
    out += ",\"history\":[";
    const auto hist_it = scoped ? s.history_by_instance.find(instance) : s.history_by_instance.end();
    const auto& history = hist_it == s.history_by_instance.end() ? s.history : hist_it->second;
    for (size_t i = 0; i < history.size(); ++i) {
        if (i) out += ",";
        const auto& h = history[i];
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

std::string DashboardServer::timeline_json(const std::string& instance,
                                           int64_t from_ms,
                                           int64_t to_ms,
                                           int64_t range_ms) const {
    auto s = snapshot();
    if (to_ms <= 0) to_ms = s.last_update_ms > 0 ? s.last_update_ms : now_ms();
    if (range_ms <= 0) range_ms = 5 * 60 * 1000;
    if (from_ms <= 0) from_ms = to_ms - range_ms;
    if (from_ms > to_ms) std::swap(from_ms, to_ms);
    const int64_t bucket_ms = choose_bucket_ms(from_ms, to_ms);
    const auto& series = timeline_series(s, instance, bucket_ms);

    std::string out = "{";
    out += json_pair("from_ms", from_ms);
    out += ",";
    out += json_pair("to_ms", to_ms);
    out += ",";
    out += json_pair("bucket_ms", bucket_ms);
    out += ",";
    out += json_pair("instance", instance);
    out += ",\"series\":[";
    bool first = true;
    for (const auto& h : series) {
        if (h.ts_ms < from_ms || h.ts_ms > to_ms) continue;
        if (!first) out += ",";
        first = false;
        out += "{";
        out += json_pair("ts_ms", h.ts_ms);
        out += ",";
        out += json_pair("rps", h.rps);
        out += ",";
        out += json_pair("bytes_per_sec", h.bytes_per_sec);
        out += ",";
        out += json_pair("samples", h.samples);
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
    out += ",\"instance_facets\":";
    out += facet_json(result.instance_facets);
    out += ",\"rows\":[";
    for (size_t i = 0; i < result.rows.size(); ++i) {
        if (i) out += ",";
        const auto& r = result.rows[i];
        out += "{";
        out += json_pair("ts_ms", r.ts_ms);
        out += ",";
        out += json_pair("ruleset_version", r.ruleset_version);
        out += ",";
        out += json_pair("instance", r.instance);
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
        out += ",\"model_scores\":{";
        for (size_t m = 0; m < r.model_scores.size(); ++m) {
            if (m) out += ",";
            out += json_pair(r.model_scores[m].first, r.model_scores[m].second);
        }
        out += "}";
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

std::string DashboardServer::models_json(int bins, const std::string& instance) const {
    std::vector<ModelHistogram> models = decision_tailer_.model_histograms(bins, instance);
    std::string out = "{\"models\":[";
    for (size_t i = 0; i < models.size(); ++i) {
        if (i) out += ",";
        const ModelHistogram& m = models[i];
        out += "{";
        out += json_pair("name", m.name);
        out += ",";
        out += json_pair("count", static_cast<double>(m.count));
        out += ",";
        out += json_pair("min", m.min);
        out += ",";
        out += json_pair("max", m.max);
        out += ",";
        out += json_pair("mean", m.mean);
        out += ",\"bins\":[";
        for (size_t b = 0; b < m.bins.size(); ++b) {
            if (b) out += ",";
            out += "{";
            out += json_pair("lo", m.bins[b].lo);
            out += ",";
            out += json_pair("hi", m.bins[b].hi);
            out += ",";
            out += json_pair("count", static_cast<double>(m.bins[b].count));
            out += "}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

std::string DashboardServer::rules_json(size_t limit, const std::string& instance) const {
    auto s = snapshot();
    DecisionState scoped_state;
    if (!instance.empty()) scoped_state = decision_tailer_.scoped_state(instance);
    const auto& winning_counts = instance.empty() ? s.decisions.winning_rule_counts
                                                   : scoped_state.winning_rule_counts;
    struct RuleRow {
        std::string id;
        double fired = 0.0;
        double fire_rate = 0.0;
        int64_t winning_total = 0;
    };
    std::map<std::string, RuleRow> rows;
    if (instance.empty()) {
        for (const auto& entry : s.metrics.entries) {
            auto it = entry.labels.find("rule_id");
            if (it == entry.labels.end()) continue;
            auto& row = rows[it->second];
            row.id = it->second;
            if (entry.name == "blazerules_rule_fired_total") row.fired = entry.value;
            if (entry.name == "blazerules_rule_fire_rate") row.fire_rate = entry.value;
        }
    }
    for (const auto& [rule_id, count] : winning_counts) {
        auto& row = rows[rule_id];
        row.id = rule_id;
        row.winning_total = count;
        if (row.fired == 0.0) row.fired = static_cast<double>(count);
    }
    const int64_t denom_rows = instance.empty() ? s.decisions.rows_seen : scoped_state.rows_seen;
    double denominator = denom_rows > 0
        ? static_cast<double>(denom_rows)
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

std::string DashboardServer::errors_json(size_t limit, const std::string& instance) const {
    auto s = snapshot();
    const ErrorState errors = scoped_errors(s.errors, instance);
    std::string out = "{\"total_recent\":";
    out += std::to_string(errors.rows_seen);
    out += ",\"code_counts\":";
    out += map_to_json(errors.code_counts);
    out += ",\"instance_counts\":";
    out += map_to_json(errors.instance_counts);
    out += ",\"rows\":[";
    size_t n = std::min(limit, errors.recent.size());
    size_t start = errors.recent.size() > n ? errors.recent.size() - n : 0;
    for (size_t i = start; i < errors.recent.size(); ++i) {
        if (i != start) out += ",";
        const auto& r = errors.recent[i];
        out += "{";
        out += json_pair("ts_ms", r.ts_ms);
        out += ",";
        out += json_pair("instance", r.instance);
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

std::string DashboardServer::ruleset_json(const std::string& selector) const {
    auto s = snapshot();
    RulesetState scoped;
    const bool use_scope = !selector.empty() && (!options_.rules_dir.empty() || !options_.rules_path.empty());
    if (use_scope) {
        scoped = ruleset_reader_.ruleset_for(selector);
        scoped.candidate_path = s.ruleset.candidate_path;
        scoped.candidate_configured = s.ruleset.candidate_configured;
        scoped.versions = s.ruleset.versions;
    }
    const RulesetState& r = use_scope ? scoped : s.ruleset;
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
    out += "],\"names\":[";
    const std::vector<std::string> names = ruleset_reader_.ruleset_names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ",";
        out += "\"";
        out += json_escape(names[i]);
        out += "\"";
    }
    out += "]}";
    return out;
}

void DashboardServer::refresh_once() {
    DashboardSnapshot next;
    next.last_update_ms = now_ms();

    if (!s3_source_.empty() || !s3_rules_source_.empty()) {
        const int64_t now = now_ms();
        if (now - last_s3_sync_ms_ >= 3000) {
            last_s3_sync_ms_ = now;
            if (s3_source_is_dir_) blazerules::s3_sync_down(s3_source_, s3_local_root_);
            else if (!s3_source_.empty()) blazerules::s3_download_file(s3_source_, s3_local_root_);
            if (!s3_rules_source_.empty()) blazerules::s3_sync_down(s3_rules_source_, s3_rules_local_);
        }
    }

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

    const auto input_by_instance = read_input_stats_by_instance(options_);
    double total_input_bytes = 0.0;
    double total_input_records = 0.0;
    for (const auto& kv : input_by_instance) {
        total_input_bytes += kv.second.bytes;
        total_input_records += kv.second.records;
        next.input_bytes_total_by_instance[kv.first] = kv.second.bytes;
        next.input_records_total_by_instance[kv.first] = kv.second.records;

        auto last_bytes_it = last_input_bytes_by_instance_.find(kv.first);
        auto bytes_ms_it = last_input_bytes_ms_by_instance_.find(kv.first);
        if (last_bytes_it != last_input_bytes_by_instance_.end() &&
            bytes_ms_it != last_input_bytes_ms_by_instance_.end() &&
            next.last_update_ms > bytes_ms_it->second && kv.second.bytes >= last_bytes_it->second) {
            next.recent_input_bytes_per_sec_by_instance[kv.first] =
                (kv.second.bytes - last_bytes_it->second) * 1000.0 /
                static_cast<double>(next.last_update_ms - bytes_ms_it->second);
        }

        auto last_records_it = last_input_records_by_instance_.find(kv.first);
        auto records_ms_it = last_input_records_ms_by_instance_.find(kv.first);
        if (last_records_it != last_input_records_by_instance_.end() &&
            records_ms_it != last_input_records_ms_by_instance_.end() &&
            next.last_update_ms > records_ms_it->second && kv.second.records >= last_records_it->second) {
            next.recent_input_records_per_sec_by_instance[kv.first] =
                (kv.second.records - last_records_it->second) * 1000.0 /
                static_cast<double>(next.last_update_ms - records_ms_it->second);
        }

        last_input_bytes_by_instance_[kv.first] = kv.second.bytes;
        last_input_bytes_ms_by_instance_[kv.first] = next.last_update_ms;
        last_input_records_by_instance_[kv.first] = kv.second.records;
        last_input_records_ms_by_instance_[kv.first] = next.last_update_ms;
    }

    if (!input_by_instance.empty()) {
        next.input_bytes_total = total_input_bytes;
        next.input_records_total = total_input_records;
        if (last_input_bytes_ms_ > 0 && next.last_update_ms > last_input_bytes_ms_ &&
            total_input_bytes >= last_input_bytes_) {
            next.recent_input_bytes_per_sec = (total_input_bytes - last_input_bytes_) * 1000.0 /
                                              static_cast<double>(next.last_update_ms - last_input_bytes_ms_);
        }
        if (last_input_records_ms_ > 0 && next.last_update_ms > last_input_records_ms_ &&
            total_input_records >= last_input_records_) {
            next.recent_input_records_per_sec = (total_input_records - last_input_records_) * 1000.0 /
                                                static_cast<double>(next.last_update_ms - last_input_records_ms_);
        }
        last_input_bytes_ = total_input_bytes;
        last_input_bytes_ms_ = next.last_update_ms;
        last_input_records_ = total_input_records;
        last_input_records_ms_ = next.last_update_ms;
    } else {
        last_input_bytes_by_instance_.clear();
        last_input_bytes_ms_by_instance_.clear();
        last_input_records_by_instance_.clear();
        last_input_records_ms_by_instance_.clear();
        last_input_bytes_ = 0.0;
        last_input_records_ = 0.0;
        last_input_bytes_ms_ = 0;
        last_input_records_ms_ = 0;
    }

    double records = metric_value(next.metrics, "blazerules_records_evaluated_total");
    if (records == 0.0) records = static_cast<double>(next.decisions.rows_seen);
    uintmax_t decision_log_bytes = 0;
    auto decision_it = next.sources.find("decision_log");
    if (decision_it != next.sources.end()) decision_log_bytes = decision_it->second.bytes;
    double discovered_rps = 0.0;
    if (records > 0.0 && last_records_ms_ > 0 && next.last_update_ms > last_records_ms_ && records >= last_records_) {
        discovered_rps = (records - last_records_) * 1000.0 /
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
    next.recent_rps = !input_by_instance.empty() ? next.recent_input_records_per_sec : discovered_rps;

    HistoryPoint hp;
    hp.ts_ms = next.last_update_ms;
    hp.rps = next.recent_rps;
    hp.bytes_per_sec = !input_by_instance.empty() ? next.recent_input_bytes_per_sec : next.recent_bytes_per_sec;
    hp.total_ms = histogram_mean_us(next.metrics, "blazerules_batch_total_latency_us") / 1000.0;
    hp.evaluation_ms = histogram_mean_us(next.metrics, "blazerules_batch_evaluation_latency_us") / 1000.0;
    hp.transpose_ms = histogram_mean_us(next.metrics, "blazerules_batch_transpose_latency_us") / 1000.0;

    std::lock_guard<std::mutex> lock(mu_);
    next.history = snapshot_.history;
    next.history_by_instance = snapshot_.history_by_instance;
    next.timeline_1s = snapshot_.timeline_1s;
    next.timeline_1m = snapshot_.timeline_1m;
    next.timeline_1h = snapshot_.timeline_1h;
    next.timeline_1s_by_instance = snapshot_.timeline_1s_by_instance;
    next.timeline_1m_by_instance = snapshot_.timeline_1m_by_instance;
    next.timeline_1h_by_instance = snapshot_.timeline_1h_by_instance;
    if (hp.rps > 0.0 || hp.bytes_per_sec > 0.0 || hp.total_ms > 0.0 || !next.metrics.entries.empty() || !next.benchmarks.rows.empty()) {
        next.history.push_back(hp);
        if (next.history.size() > 180) {
            next.history.erase(next.history.begin(), next.history.begin() + static_cast<long>(next.history.size() - 180));
        }
        append_timeline_buckets(next.timeline_1s, next.timeline_1m, next.timeline_1h, hp);
    }
    for (const auto& kv : input_by_instance) {
        HistoryPoint ihp;
        ihp.ts_ms = next.last_update_ms;
        auto rps_it = next.recent_input_records_per_sec_by_instance.find(kv.first);
        auto bps_it = next.recent_input_bytes_per_sec_by_instance.find(kv.first);
        ihp.rps = rps_it == next.recent_input_records_per_sec_by_instance.end() ? 0.0 : rps_it->second;
        ihp.bytes_per_sec = bps_it == next.recent_input_bytes_per_sec_by_instance.end() ? 0.0 : bps_it->second;
        if (ihp.rps <= 0.0 && ihp.bytes_per_sec <= 0.0) continue;
        auto& history = next.history_by_instance[kv.first];
        history.push_back(ihp);
        if (history.size() > 180) {
            history.erase(history.begin(), history.begin() + static_cast<long>(history.size() - 180));
        }
        append_timeline_buckets(next.timeline_1s_by_instance[kv.first],
                                next.timeline_1m_by_instance[kv.first],
                                next.timeline_1h_by_instance[kv.first],
                                ihp);
    }
    if (input_by_instance.empty()) {
        next.history_by_instance.clear();
        next.timeline_1s_by_instance.clear();
        next.timeline_1m_by_instance.clear();
        next.timeline_1h_by_instance.clear();
    }
    snapshot_ = std::move(next);
}
