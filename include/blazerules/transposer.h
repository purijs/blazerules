#ifndef BLAZERULES_TRANSPOSER_H
#define BLAZERULES_TRANSPOSER_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/api.h>
#include <simdjson.h>

#include "batch_result.h"
#include "kernel_sequence.h"
#include "schema.h"

class BatchTransposer {
public:
    explicit BatchTransposer(const BlazeRulesSchema& schema,
                             arrow::MemoryPool* pool = arrow::default_memory_pool());

    void reset();
    void reserve(int rows);
    void set_projected_fields(std::vector<int> projected_indices);
    void add_json_message(std::string_view json_bytes);
    void add_json_messages(const std::vector<std::string_view>& messages);
    void add_ndjson(std::string_view ndjson_bytes, int thread_count = 0);
    void add_ndjson_padded(std::string_view ndjson_bytes, int thread_count = 0);
    void set_max_error_samples(int max_samples);
    void set_array_any_channels(std::vector<ArrayAnyChannelSpec> channels,
                                LookupRegistry lookups = {});
    int current_size() const { return current_size_; }
    int skipped_count() const { return skipped_count_; }
    const std::string& last_error() const { return last_error_; }
    const absl::flat_hash_map<std::string, int>& error_counts() const { return error_counts_; }
    const std::vector<BatchErrorSample>& error_samples() const { return error_samples_; }
    std::shared_ptr<arrow::RecordBatch> finish();

private:
    struct ColumnBuffer {
        ColumnType type = ColumnType::FLOAT64;
        int64_t length = 0;
        int64_t null_count = 0;
        std::vector<float> f32;
        std::vector<double> f64;
        std::vector<int32_t> i32;
        std::vector<int64_t> i64;
        std::vector<uint8_t> bool_bits;
        std::vector<uint8_t> validity_bits;
        std::vector<int32_t> offsets;
        std::string string_data;

        void reset();
        void reserve(int rows);
    };

    void make_buffers();
    bool should_materialize(int col_index) const;
    void rebuild_projected_indices();
    template <typename DocumentLike>
    bool append_json_document(DocumentLike& doc);
    template <typename DocumentLike>
    bool append_json_document_single_pass(DocumentLike& doc);
    void append_json_object_fields(simdjson::ondemand::object& object, std::string& prefix);
    void append_json_value(int col_index, simdjson::ondemand::value& value);
    void append_json_value_if_new(int col_index, simdjson::ondemand::value& value);
    void append_default(int col_index);
    void append_null(int col_index);
    void append_int32_literal(int col_index, int32_t value);
    bool append_array_any_value(const std::string& path, simdjson::ondemand::value& value);
    bool mark_column_seen(int col_index);
    void parse_ndjson_view(std::string_view ndjson_bytes);
    void parse_ndjson_lines_safe(std::string_view ndjson_bytes);
    void parse_ndjson_parallel(std::string_view ndjson_bytes, int thread_count);
    void record_error(std::string code, std::string message, std::string source,
                      int64_t row_index, std::string column_name, bool skip_record);
    void record_type_error(int col_index, int64_t row_index);
    void learn_field_order_from_ndjson(std::string_view ndjson_bytes);
    std::vector<std::pair<size_t, size_t>> split_ndjson(std::string_view ndjson_bytes,
                                                        int thread_count) const;
    void merge_from(BatchTransposer& other);
    std::shared_ptr<arrow::Array> finish_column(int col_index);
    std::shared_ptr<arrow::Array> finish_unprojected_column(int col_index) const;

    const BlazeRulesSchema& schema_;
    arrow::MemoryPool* pool_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
    simdjson::ondemand::parser parser_;
    std::vector<ColumnBuffer> columns_;
    absl::flat_hash_map<std::string, int> field_to_index_;
    std::vector<uint8_t> direct_encoded_;
    std::vector<absl::flat_hash_map<std::string, int32_t>> closed_value_ids_;
    std::vector<uint8_t> projected_;
    std::vector<int> projected_indices_;
    std::vector<ArrayAnyChannelSpec> array_any_channels_;
    LookupRegistry array_any_lookups_;
    std::vector<int> learned_field_order_;
    // The key at each position of the learned order, used to VALIDATE that a document's
    // fields actually appear in that order before trusting the positional column mapping.
    // Without this, NDJSON rows whose key order differs from the first row would be
    // silently misassigned; on a per-position key mismatch we fall back to the
    // field_to_index_ hash lookup (the common homogeneous case stays on the fast path).
    std::vector<std::string> learned_field_keys_;
    bool layout_is_flat_ = false;
    std::vector<uint32_t> seen_generation_;
    std::string padded_json_;
    std::string batch_json_;
    int current_size_ = 0;
    int skipped_count_ = 0;
    int max_error_samples_ = 16;
    uint32_t row_generation_ = 1;
    int64_t input_row_index_ = 0;
    int seen_count_ = 0;
    std::string last_error_;
    absl::flat_hash_map<std::string, int> error_counts_;
    std::vector<BatchErrorSample> error_samples_;
};

#endif // BLAZERULES_TRANSPOSER_H
