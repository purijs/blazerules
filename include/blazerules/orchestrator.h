#ifndef BLAZERULES_ORCHESTRATOR_H
#define BLAZERULES_ORCHESTRATOR_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <arrow/api.h>
#include "kernel_sequence.h"
#include "batch_result.h"

struct EvalOptions {
    int parallel_threshold = 1000;
    bool enable_selection_vectors = true;
    double selection_vector_threshold = 0.20;
    bool enable_adaptive_predicate_ordering = true;
    bool enable_no_validity_fast_path = true;
    bool enable_prefetch = false;
    bool enable_thread_affinity = false;
    bool result_buffer_reuse = true;
    bool materialize_rule_bitmasks = true;
    bool materialize_matched_indices = true;
    bool materialize_decision_codes = true;
    bool materialize_decision_strings = true;
    bool materialize_scores = true;
    bool materialize_risk_bands = true;
    bool materialize_winning_rules = true;
    bool materialize_grouped_indices = true;
    bool materialize_model_outputs = true;
    size_t arena_size_bytes = 8 * 1024 * 1024;
};

struct ResolvedKernelBindings {
    std::vector<std::vector<std::vector<int32_t>>> per_rule;
    std::vector<std::vector<int32_t>> global_predicates;
};

void evaluate_rules(const arrow::RecordBatch& batch,
                    const CompiledRuleSet& rs,
                    const ResolvedKernelBindings& resolved,
                    const EvalOptions& options,
                    BatchResult& out);

#endif //BLAZERULES_ORCHESTRATOR_H
