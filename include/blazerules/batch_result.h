#ifndef BLAZERULES_BATCH_RESULT_H
#define BLAZERULES_BATCH_RESULT_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/buffer.h>

#include "rule_spec.h"

struct BatchErrorSample {
    std::string code;
    std::string message;
    std::string source;
    int64_t row_index = -1;
    std::string column_name;
};

struct ConditionExplanation {
    std::string field_name;
    std::string operator_str;
    std::string threshold_str;
    std::string actual_value;
    bool satisfied = true;
};

struct RuleFiringExplanation {
    std::string rule_id;
    std::string rule_name;
    ActionType action = ActionType::FLAG;
    std::vector<ConditionExplanation> satisfied_conditions;
};

struct RecordExplanation {
    int32_t record_index = 0;
    std::vector<RuleFiringExplanation> fired_rules;
};

struct BatchResult {
    int n_records = 0;
    int n_matched = 0;

    absl::flat_hash_map<std::string, std::vector<uint8_t>> rule_bitmasks;
    absl::flat_hash_map<std::string, int> rule_match_counts;
    std::vector<int32_t> matched_record_indices;
    absl::flat_hash_map<int32_t, RecordExplanation> explanations;
    std::vector<std::string> decisions;
    std::vector<int32_t> decision_codes;
    std::vector<std::string> decision_labels;
    std::vector<double> scores;
    std::vector<std::string> risk_bands;
    std::vector<std::string> winning_rule_ids;
    absl::flat_hash_map<std::string, std::vector<int32_t>> grouped_decision_indices;
    absl::flat_hash_map<std::string, std::vector<int32_t>> grouped_winning_rule_indices;

    std::string rule_set_version;
    int64_t evaluation_timestamp_ms = 0;
    int messages_processed = 0;
    int messages_skipped = 0;
    std::string last_ingest_error;
    absl::flat_hash_map<std::string, int> error_counts;
    std::vector<BatchErrorSample> error_samples;

    struct Timing {
        int64_t transpose_us = 0;
        int64_t dict_encode_us = 0;
        int64_t window_read_us = 0;
        int64_t window_inject_us = 0;
        int64_t window_write_us = 0;
        int64_t model_score_us = 0;
        int64_t kernel_bind_us = 0;
        int64_t evaluation_us = 0;
        int64_t result_assemble_us = 0;
        int64_t total_us = 0;
    };
    Timing timing;

    std::shared_ptr<arrow::Buffer> rule_bitmask_buffer(const std::string& rule_id) const {
        auto it = rule_bitmasks.find(rule_id);
        if (it == rule_bitmasks.end() || it->second.empty()) return nullptr;
        return std::make_shared<arrow::Buffer>(it->second.data(),
                                               static_cast<int64_t>(it->second.size()));
    }

    std::shared_ptr<arrow::Buffer> matched_indices_buffer() const {
        if (matched_record_indices.empty()) return nullptr;
        const auto* ptr = reinterpret_cast<const uint8_t*>(matched_record_indices.data());
        return std::make_shared<arrow::Buffer>(
            ptr, static_cast<int64_t>(matched_record_indices.size() * sizeof(int32_t)));
    }

    std::shared_ptr<arrow::Buffer> decision_codes_buffer() const {
        if (decision_codes.empty()) return nullptr;
        const auto* ptr = reinterpret_cast<const uint8_t*>(decision_codes.data());
        return std::make_shared<arrow::Buffer>(
            ptr, static_cast<int64_t>(decision_codes.size() * sizeof(int32_t)));
    }

    std::shared_ptr<arrow::Buffer> grouped_indices_buffer(const std::string& decision) const {
        auto it = grouped_decision_indices.find(decision);
        if (it == grouped_decision_indices.end() || it->second.empty()) return nullptr;
        const auto* ptr = reinterpret_cast<const uint8_t*>(it->second.data());
        return std::make_shared<arrow::Buffer>(
            ptr, static_cast<int64_t>(it->second.size() * sizeof(int32_t)));
    }

    std::unordered_map<std::string, double> timing_ms() const {
        return {
            {"transpose", timing.transpose_us / 1000.0},
            {"dict_encode", timing.dict_encode_us / 1000.0},
            {"window_read", timing.window_read_us / 1000.0},
            {"window_inject", timing.window_inject_us / 1000.0},
            {"window_write", timing.window_write_us / 1000.0},
            {"model_score", timing.model_score_us / 1000.0},
            {"kernel_bind", timing.kernel_bind_us / 1000.0},
            {"evaluation", timing.evaluation_us / 1000.0},
            {"result_assemble", timing.result_assemble_us / 1000.0},
            {"total", timing.total_us / 1000.0},
        };
    }
};

struct BacktestReport {
    std::string rule_set_a_name;
    std::string rule_set_b_name;
    int64_t total_records = 0;
    double fire_rate_a = 0.0;
    double fire_rate_b = 0.0;
    int64_t new_positives = 0;
    int64_t lost_positives = 0;
    double agreement_rate = 0.0;
    double precision_a = 0.0;
    double recall_a = 0.0;
    double precision_b = 0.0;
    double recall_b = 0.0;
    absl::flat_hash_map<std::string, double> per_rule_fire_rates;
};

#endif // BLAZERULES_BATCH_RESULT_H
