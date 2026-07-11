#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    std::string instance;
    int64_t batch_row = 0;
    bool matched = false;
    std::string decision;
    double score = 0.0;
    std::string risk_band;
    std::string winning_rule_id;
    std::vector<std::pair<std::string, double>> model_scores;
};

struct DecisionState {
    std::vector<DecisionRow> recent;
    std::map<std::string, int64_t> decision_counts;
    std::map<std::string, int64_t> risk_band_counts;
    std::map<std::string, int64_t> winning_rule_counts;
    std::map<std::string, int64_t> instance_counts;
    std::map<std::string, int64_t> instance_log_bytes;
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
    double bytes_per_sec = 0.0;
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
    double recent_bytes_per_sec = 0.0;
    double recent_input_bytes_per_sec = 0.0;
};

struct ModelHistogramBin {
    double lo = 0.0;
    double hi = 0.0;
    int64_t count = 0;
};

struct ModelHistogram {
    std::string name;
    int64_t count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    std::vector<ModelHistogramBin> bins;
};

struct DecisionQuery {
    size_t limit = 500;
    size_t offset = 0;
    bool scan_file = false;
    std::string decision;
    std::string risk_band;
    std::string rule;
    std::string instance;
    int64_t from_ms = 0;
    int64_t to_ms = 0;
};

struct DecisionQueryResult {
    std::vector<DecisionRow> rows;
    int64_t total_matches = 0;
    int64_t indexed_rows = 0;
    bool truncated = false;
    std::map<std::string, int64_t> decision_facets;
    std::map<std::string, int64_t> risk_band_facets;
    std::map<std::string, int64_t> instance_facets;
};

struct Options {
    std::string host = "127.0.0.1";
    int port = 9470;
    int poll_ms = 1000;
    size_t tail_lines = 5000;
    size_t max_index_rows = 5000000;
    std::string decision_log;
    std::string decision_log_dir;
    std::string dead_letter_log;
    std::string metrics_url;
    std::string results_jsonl;
    std::string rules_path;
    std::string rules_dir;
    std::string candidate_rules_path;
    std::string rules_history_dir;
    std::string aws_region;
    std::string aws_endpoint_url;
};
