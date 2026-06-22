#ifndef BLAZERULES_WINDOW_STORE_H
#define BLAZERULES_WINDOW_STORE_H

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/api.h>

#include "kernel_sequence.h"

struct EntityCounter {
    absl::flat_hash_map<int64_t, double> extra_buckets;
    absl::flat_hash_map<int64_t, double> extra_count_buckets;
    int64_t inline_bucket_second = 0;
    double inline_bucket_value = 0.0;
    double inline_bucket_count = 0.0;
    double running_total = 0.0;
    double running_count = 0.0;
    uint32_t write_head = 0;
    int64_t last_write_second = 0;
    bool has_inline_bucket = false;
    bool initialized = false;
};

struct WindowChannel {
    int entity_col_index = 0;
    std::string entity_field;
    WindowFn function = WindowFn::COUNT;
    int sum_col_index = -1;
    std::string sum_field;
    int denominator_col_index = -1;
    std::string denominator_field;
    int duration_seconds = 0;
    int injected_col_index = 0;
    std::string injected_name;
    ColumnType column_type = ColumnType::INT32;
    // Dense per-entity storage indexed directly by the entity's dictionary id
    // (ids are dense 0..N from the DictEncoder). Avoids 5M+ hash lookups/rehashes
    // per batch vs a flat_hash_map keyed by id.
    std::vector<EntityCounter> entities;
    int64_t active_entities = 0;
};

class WindowStore {
public:
    void configure(const std::vector<WindowChannelSpec>& specs);
    void query_all_channels(const arrow::RecordBatch& encoded, int64_t now_seconds,
                            std::vector<std::vector<double>>& out);
    void update_all_channels(const arrow::RecordBatch& encoded, int64_t now_seconds);
    void evict_stale_entities(int64_t current_time_seconds);
    void reset_all();
    size_t memory_usage_bytes() const;
    int num_channels() const { return static_cast<int>(channels_.size()); }
    int num_active_entities(int channel_index) const;
    const std::vector<WindowChannel>& channels() const { return channels_; }

private:
    std::vector<WindowChannel> channels_;
    // Reused dense group-by scratch (indexed by entity dict id) with a
    // generation stamp so we never re-zero between batches. Replaces a
    // per-batch flat_hash_map for coalescing rows of the same entity.
    std::vector<double> delta_scratch_;
    std::vector<double> count_scratch_;
    std::vector<uint32_t> stamp_;
    std::vector<int32_t> touched_;
    uint32_t stamp_gen_ = 0;
};

// Append every derived column (windows, ML scores, vector distances, time-series
// aggregates) to the batch in a single rebuild, in injected-index (== slot) order.
// slot_values[k] points at `num_rows` doubles for plan.slots[k] (may be null -> zeros);
// the value is narrowed to the slot's FLOAT32/INT32 column type. Arrays are built with
// arrow::ArrayData::Make over a filled buffer (no per-row arrow::Builder Append).
std::shared_ptr<arrow::RecordBatch> inject_derived_columns(
    const std::shared_ptr<arrow::RecordBatch>& encoded,
    const DerivedColumnPlan& plan,
    const std::vector<const double*>& slot_values);

#endif // BLAZERULES_WINDOW_STORE_H
