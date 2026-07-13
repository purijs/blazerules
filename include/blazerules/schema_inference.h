#ifndef BLAZERULES_SCHEMA_INFERENCE_H
#define BLAZERULES_SCHEMA_INFERENCE_H

#include <memory>
#include <string_view>
#include <vector>

#include <arrow/api.h>

#include "result.h"
#include "rule_spec.h"
#include "schema.h"

struct SchemaInferenceOptions {
    int64_t sample_rows = 20480;
    int64_t sample_bytes = 64 * 1024 * 1024;
    int max_depth = 16;
    int categorical_max_cardinality = 4096;
};

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_messages(
    const RuleFileSpec& rules,
    const std::vector<std::string_view>& messages,
    const SchemaInferenceOptions& options = {});

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_ndjson(
    const RuleFileSpec& rules,
    std::string_view ndjson,
    const SchemaInferenceOptions& options = {});

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_json_array(
    const RuleFileSpec& rules,
    std::string_view json_array,
    bool padded,
    const SchemaInferenceOptions& options = {});

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_arrow(
    const RuleFileSpec& rules,
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const SchemaInferenceOptions& options = {});

#endif // BLAZERULES_SCHEMA_INFERENCE_H
