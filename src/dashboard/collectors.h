#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "state.h"

namespace arrow {
class Schema;
class RecordBatch;
namespace ipc {
class DictionaryMemo;
}
}  // namespace arrow

struct LogSource {
    std::string path;
    std::string instance_label;
    uintmax_t read_offset = 0;
    bool format_detected = false;
    bool arrow_mode = false;
    std::shared_ptr<arrow::Schema> arrow_schema;
    std::shared_ptr<arrow::ipc::DictionaryMemo> dict_memo;
};

struct InstanceAgg {
    int64_t rows = 0;
    int64_t matched = 0;
    double score_sum = 0.0;
    std::map<std::string, int64_t> decision_counts;
    std::map<std::string, int64_t> risk_counts;
    std::map<std::string, int64_t> rule_counts;
};

class DecisionLogTailer {
public:
    DecisionLogTailer(std::string root, bool is_dir, size_t tail_lines, size_t index_capacity);
    ~DecisionLogTailer();
    DecisionState update(SourceStatus& status);
    DecisionQueryResult query(const DecisionQuery& q) const;
    std::vector<std::string> model_columns() const;
    std::vector<ModelHistogram> model_histograms(int bins, const std::string& instance) const;
    DecisionState scoped_state(const std::string& instance) const;

private:
    int32_t intern_label(std::vector<std::string>& labels,
                         std::unordered_map<std::string, int32_t>& ids,
                         const std::string& value);
    void index_push(const DecisionRow& row);
    void index_reset();
    void ingest_row(DecisionRow&& row);
    void read_ndjson(LogSource& src, uintmax_t bytes);
    void read_arrow(LogSource& src, SourceStatus& status);
    void index_arrow_batch(const arrow::RecordBatch& batch, const std::string& fallback_instance);
    void discover_sources();

    std::string root_;
    bool is_dir_ = false;
    std::vector<LogSource> sources_;
    size_t tail_lines_ = 5000;
    double score_sum_ = 0.0;
    DecisionState cached_;
    std::deque<DecisionRow> recent_;

    mutable std::shared_mutex index_mu_;
    size_t index_capacity_ = 0;
    size_t index_head_ = 0;
    size_t index_count_ = 0;
    int64_t index_total_ = 0;
    std::vector<int64_t> idx_ts_ms_;
    std::vector<float> idx_score_;
    std::vector<int32_t> idx_batch_row_;
    std::vector<uint8_t> idx_matched_;
    std::vector<int32_t> idx_decision_;
    std::vector<int32_t> idx_risk_;
    std::vector<int32_t> idx_rule_;
    std::vector<int32_t> idx_version_;
    std::vector<int32_t> idx_instance_;
    std::vector<std::string> decision_labels_;
    std::vector<std::string> risk_labels_;
    std::vector<std::string> rule_labels_;
    std::vector<std::string> version_labels_;
    std::vector<std::string> instance_labels_;
    std::unordered_map<std::string, int32_t> decision_ids_;
    std::unordered_map<std::string, int32_t> risk_ids_;
    std::unordered_map<std::string, int32_t> rule_ids_;
    std::unordered_map<std::string, int32_t> version_ids_;
    std::unordered_map<std::string, int32_t> instance_ids_;
    std::vector<std::string> model_columns_;
    std::unordered_map<std::string, size_t> model_column_ids_;
    std::vector<std::vector<float>> idx_model_scores_;
    std::unordered_map<std::string, InstanceAgg> instance_agg_;
};

class DeadLetterTailer {
public:
    DeadLetterTailer(std::string root, bool is_dir, size_t tail_lines);
    ErrorState update(SourceStatus& status);

private:
    void read_file(const std::string& path, uintmax_t& offset);
    std::string root_;
    bool is_dir_ = false;
    size_t tail_lines_ = 5000;
    std::map<std::string, uintmax_t> offsets_;
    ErrorState cached_;
    std::deque<ErrorRow> recent_;
};

class PrometheusScraper {
public:
    explicit PrometheusScraper(std::string url);
    MetricsState update(SourceStatus& status);

private:
    std::string url_;
    MetricsState cached_;
};

class BenchmarkReader {
public:
    explicit BenchmarkReader(std::string path);
    BenchmarkState update(SourceStatus& status);

private:
    std::string path_;
    uintmax_t last_size_ = 0;
    int64_t last_mtime_count_ = 0;
    BenchmarkState cached_;
};

class RulesetReader {
public:
    RulesetReader(std::string active_path, std::string rules_dir,
                  std::string candidate_path, std::string history_dir);
    RulesetState update(SourceStatus& status);
    // Parse and return the ruleset matching `selector` (an instance/ruleset name).
    // When a rules_dir is configured, picks the *.yaml whose stem best matches;
    // otherwise returns the single active ruleset. Self-contained (no shared state),
    // safe to call concurrently from request handlers.
    RulesetState ruleset_for(const std::string& selector) const;
    std::vector<std::string> ruleset_names() const;

private:
    std::string resolve_selector_path(const std::string& selector) const;
    std::string active_path_;
    std::string rules_dir_;
    std::string candidate_path_;
    std::string history_dir_;
    RulesetState cached_;
};
