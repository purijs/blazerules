#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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

class DecisionLogTailer {
public:
    DecisionLogTailer(std::string path, size_t tail_lines, size_t index_capacity);
    ~DecisionLogTailer();
    DecisionState update(SourceStatus& status);
    DecisionQueryResult query(const DecisionQuery& q) const;

private:
    int32_t intern_label(std::vector<std::string>& labels,
                         std::unordered_map<std::string, int32_t>& ids,
                         const std::string& value);
    void index_push(const DecisionRow& row);
    void index_reset();
    void ingest_row(DecisionRow&& row);
    void read_ndjson(uintmax_t bytes);
    void read_arrow(SourceStatus& status);
    void index_arrow_batch(const arrow::RecordBatch& batch);

    std::string path_;
    size_t tail_lines_ = 5000;
    uintmax_t read_offset_ = 0;
    double score_sum_ = 0.0;
    DecisionState cached_;
    std::deque<DecisionRow> recent_;
    bool format_detected_ = false;
    bool arrow_mode_ = false;
    std::shared_ptr<arrow::Schema> arrow_schema_;
    std::unique_ptr<arrow::ipc::DictionaryMemo> dict_memo_;

    mutable std::mutex index_mu_;
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
    std::vector<std::string> decision_labels_;
    std::vector<std::string> risk_labels_;
    std::vector<std::string> rule_labels_;
    std::vector<std::string> version_labels_;
    std::unordered_map<std::string, int32_t> decision_ids_;
    std::unordered_map<std::string, int32_t> risk_ids_;
    std::unordered_map<std::string, int32_t> rule_ids_;
    std::unordered_map<std::string, int32_t> version_ids_;
};

class DeadLetterTailer {
public:
    DeadLetterTailer(std::string path, size_t tail_lines);
    ErrorState update(SourceStatus& status);

private:
    std::string path_;
    size_t tail_lines_ = 5000;
    uintmax_t read_offset_ = 0;
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
    RulesetReader(std::string active_path, std::string candidate_path, std::string history_dir);
    RulesetState update(SourceStatus& status);

private:
    std::string active_path_;
    std::string candidate_path_;
    std::string history_dir_;
    RulesetState cached_;
};
