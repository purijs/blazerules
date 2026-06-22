#include "blazerules/window_store.h"

#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <vector>

namespace {

void clear_buckets(EntityCounter& ec) {
    ec.extra_buckets.clear();
    ec.extra_count_buckets.clear();
    ec.inline_bucket_second = 0;
    ec.inline_bucket_value = 0.0;
    ec.inline_bucket_count = 0.0;
    ec.has_inline_bucket = false;
}

// COUNT/SUM/AVG/RATIO maintain an additive running total over time buckets. MIN/MAX are
// NOT additive (you can't subtract an expiring max), so their per-bucket value is the
// extreme for that second and the window result is reduced over surviving buckets at
// query time; running_total is unused for them.
inline bool is_additive_fn(WindowFn fn) {
    return fn != WindowFn::MIN && fn != WindowFn::MAX;
}

inline double combine_fn(WindowFn fn, double acc, double v) {
    if (fn == WindowFn::MIN) return v < acc ? v : acc;
    if (fn == WindowFn::MAX) return v > acc ? v : acc;
    return acc + v;  // additive
}

void add_bucket(EntityCounter& ec, int64_t second, double delta, double count_delta, WindowFn fn) {
    if (!ec.has_inline_bucket) {
        ec.inline_bucket_second = second;
        ec.inline_bucket_value = delta;
        ec.inline_bucket_count = count_delta;
        ec.has_inline_bucket = true;
        return;
    }
    if (ec.inline_bucket_second == second) {
        ec.inline_bucket_value = combine_fn(fn, ec.inline_bucket_value, delta);
        ec.inline_bucket_count += count_delta;
        return;
    }
    auto it = ec.extra_buckets.find(second);
    if (it == ec.extra_buckets.end()) {
        ec.extra_buckets[second] = delta;
        ec.extra_count_buckets[second] = count_delta;
    } else {
        it->second = combine_fn(fn, it->second, delta);
        ec.extra_count_buckets[second] += count_delta;
    }
}

void advance_and_zero(EntityCounter& ec, int64_t now, int duration, bool additive) {
    if (!ec.initialized) {
        clear_buckets(ec);
        ec.running_total = 0.0;
        ec.running_count = 0.0;
        ec.write_head = static_cast<uint32_t>(now % duration);
        ec.last_write_second = now;
        ec.initialized = true;
        return;
    }
    int64_t elapsed = now - ec.last_write_second;
    if (elapsed <= 0) return;
    if (elapsed >= duration) {
        clear_buckets(ec);
        ec.running_total = 0.0;
        ec.running_count = 0.0;
        ec.write_head = static_cast<uint32_t>(now % duration);
        ec.last_write_second = now;
        return;
    }
    int64_t cutoff = now - duration;
    if (ec.has_inline_bucket && ec.inline_bucket_second <= cutoff) {
        if (additive) {
            ec.running_total -= ec.inline_bucket_value;
            ec.running_count -= ec.inline_bucket_count;
        }
        ec.inline_bucket_second = 0;
        ec.inline_bucket_value = 0.0;
        ec.inline_bucket_count = 0.0;
        ec.has_inline_bucket = false;
    }
    for (auto it = ec.extra_buckets.begin(); it != ec.extra_buckets.end();) {
        if (it->first <= cutoff) {
            auto count_it = ec.extra_count_buckets.find(it->first);
            if (additive) {
                ec.running_total -= it->second;
                if (count_it != ec.extra_count_buckets.end()) ec.running_count -= count_it->second;
            }
            if (count_it != ec.extra_count_buckets.end()) ec.extra_count_buckets.erase(count_it);
            auto doomed = it++;
            ec.extra_buckets.erase(doomed);
        } else {
            ++it;
        }
    }
    ec.write_head = static_cast<uint32_t>(now % duration);
    ec.last_write_second = now;
}

// Typed column reader resolved once per group (avoids a std::static_pointer_cast
// and virtual dispatch on every row, which was real atomic-refcount churn).
struct ColumnReader {
    arrow::Type::type type = arrow::Type::NA;
    const uint8_t* values = nullptr;
    const uint8_t* validity = nullptr;
    int64_t offset = 0;
    bool has_nulls = false;

    bool is_null(int row) const {
        if (!validity) return false;
        int64_t r = offset + row;
        return ((validity[r >> 3] >> (r & 7)) & 1u) == 0;
    }
    double value(int row) const {
        int64_t r = offset + row;
        switch (type) {
            case arrow::Type::FLOAT: return reinterpret_cast<const float*>(values)[r];
            case arrow::Type::DOUBLE: return reinterpret_cast<const double*>(values)[r];
            case arrow::Type::INT32: return reinterpret_cast<const int32_t*>(values)[r];
            case arrow::Type::INT64: return static_cast<double>(reinterpret_cast<const int64_t*>(values)[r]);
            default: return 0.0;
        }
    }
};

ColumnReader make_reader(const std::shared_ptr<arrow::Array>& arr) {
    ColumnReader rd;
    if (!arr) return rd;
    const auto& ad = arr->data();
    rd.type = arr->type_id();
    rd.values = ad->buffers.size() > 1 && ad->buffers[1] ? ad->buffers[1]->data() : nullptr;
    rd.has_nulls = ad->GetNullCount() > 0;
    rd.validity = rd.has_nulls && !ad->buffers.empty() && ad->buffers[0] ? ad->buffers[0]->data() : nullptr;
    rd.offset = ad->offset;
    return rd;
}

struct UpdateGroup {
    int entity_col_index = 0;
    WindowFn function = WindowFn::COUNT;
    int sum_col_index = -1;
    int denominator_col_index = -1;
    std::vector<int> channel_indices;
    absl::flat_hash_map<int32_t, double> deltas;
    absl::flat_hash_map<int32_t, double> count_deltas;
};

bool same_group(const UpdateGroup& group, const WindowChannel& ch) {
    return group.entity_col_index == ch.entity_col_index &&
           group.function == ch.function &&
           group.sum_col_index == ch.sum_col_index &&
           group.denominator_col_index == ch.denominator_col_index;
}

std::vector<UpdateGroup> build_update_groups(const std::vector<WindowChannel>& channels) {
    std::vector<UpdateGroup> groups;
    groups.reserve(channels.size());
    for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
        const WindowChannel& ch = channels[i];
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const UpdateGroup& group) { return same_group(group, ch); });
        if (it == groups.end()) {
            UpdateGroup group;
            group.entity_col_index = ch.entity_col_index;
            group.function = ch.function;
            group.sum_col_index = ch.sum_col_index;
            group.denominator_col_index = ch.denominator_col_index;
            group.channel_indices.push_back(i);
            groups.push_back(std::move(group));
        } else {
            it->channel_indices.push_back(i);
        }
    }
    return groups;
}

} // namespace

void WindowStore::configure(const std::vector<WindowChannelSpec>& specs) {
    channels_.clear();
    channels_.reserve(specs.size());
    for (const auto& s : specs) {
        WindowChannel ch;
        ch.entity_col_index = s.entity_col_index;
        ch.entity_field = s.entity_field;
        ch.function = s.function;
        ch.sum_col_index = s.sum_col_index;
        ch.sum_field = s.sum_field;
        ch.denominator_col_index = s.denominator_col_index;
        ch.denominator_field = s.denominator_field;
        ch.duration_seconds = s.duration_seconds;
        ch.injected_col_index = s.injected_col_index;
        ch.injected_name = s.injected_name;
        ch.column_type = s.column_type;
        channels_.push_back(std::move(ch));
    }
}

void WindowStore::query_all_channels(const arrow::RecordBatch& encoded, int64_t now,
                                     std::vector<std::vector<double>>& out) {
    int n = static_cast<int>(encoded.num_rows());
    out.assign(channels_.size(), std::vector<double>(n, 0.0));
    for (size_t c = 0; c < channels_.size(); ++c) {
        WindowChannel& ch = channels_[c];
        const bool additive = is_additive_fn(ch.function);
        auto col = std::static_pointer_cast<arrow::Int32Array>(encoded.column(ch.entity_col_index));
        const int32_t* ids = col->raw_values();
        bool col_has_nulls = col->null_count() > 0;
        int64_t entity_size = static_cast<int64_t>(ch.entities.size());
        for (int i = 0; i < n; ++i) {
            if (col_has_nulls && col->IsNull(i)) continue;
            int32_t id = ids[i];
            if (id < 0 || id >= entity_size) continue;
            EntityCounter& ec = ch.entities[id];
            if (!ec.initialized) continue;
            advance_and_zero(ec, now, ch.duration_seconds, additive);
            if (ch.function == WindowFn::AVG || ch.function == WindowFn::RATIO) {
                out[c][i] = ec.running_count > 0.0
                    ? ec.running_total / ec.running_count
                    : 0.0;
            } else if (ch.function == WindowFn::MIN || ch.function == WindowFn::MAX) {
                // Reduce the extreme over surviving buckets (advance_and_zero already
                // dropped expired ones). No running aggregate is maintained for MIN/MAX.
                bool any = false;
                double acc = 0.0;
                if (ec.has_inline_bucket) { acc = ec.inline_bucket_value; any = true; }
                for (const auto& kv : ec.extra_buckets) {
                    acc = any ? combine_fn(ch.function, acc, kv.second) : kv.second;
                    any = true;
                }
                out[c][i] = any ? acc : 0.0;
            } else {
                out[c][i] = ec.running_total;
            }
        }
    }
}

void WindowStore::update_all_channels(const arrow::RecordBatch& encoded, int64_t now) {
    int n = static_cast<int>(encoded.num_rows());
    std::vector<UpdateGroup> groups = build_update_groups(channels_);

    for (auto& group : groups) {
        auto entity_col = std::static_pointer_cast<arrow::Int32Array>(encoded.column(group.entity_col_index));
        const int32_t* ids = entity_col->raw_values();
        bool entity_has_nulls = entity_col->null_count() > 0;

        const bool additive = is_additive_fn(group.function);
        bool need_sum = (group.function == WindowFn::SUM || group.function == WindowFn::AVG ||
                         group.function == WindowFn::RATIO || group.function == WindowFn::MIN ||
                         group.function == WindowFn::MAX) && group.sum_col_index >= 0;
        bool need_denom = group.function == WindowFn::RATIO && group.denominator_col_index >= 0;
        ColumnReader sum_reader = need_sum ? make_reader(encoded.column(group.sum_col_index)) : ColumnReader{};
        ColumnReader denom_reader = need_denom ? make_reader(encoded.column(group.denominator_col_index)) : ColumnReader{};

        int32_t max_id = -1;
        for (int i = 0; i < n; ++i) {
            if (entity_has_nulls && entity_col->IsNull(i)) continue;
            if (ids[i] > max_id) max_id = ids[i];
        }
        if (max_id < 0) continue;

        // Dense coalesce of this batch's rows per entity, generation-stamped so
        // the scratch is never re-zeroed between batches.
        size_t cap = static_cast<size_t>(max_id) + 1;
        if (delta_scratch_.size() < cap) {
            delta_scratch_.resize(cap);
            count_scratch_.resize(cap);
            stamp_.resize(cap, 0);
        }
        if (++stamp_gen_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            stamp_gen_ = 1;
        }
        touched_.clear();

        for (int i = 0; i < n; ++i) {
            if (entity_has_nulls && entity_col->IsNull(i)) continue;
            int32_t id = ids[i];
            if (id < 0) continue;
            double delta = (need_sum && !sum_reader.is_null(i)) ? sum_reader.value(i) : 1.0;
            double count_delta = (need_denom && !denom_reader.is_null(i)) ? denom_reader.value(i) : 1.0;
            if (stamp_[id] != stamp_gen_) {
                stamp_[id] = stamp_gen_;
                delta_scratch_[id] = delta;
                count_scratch_[id] = count_delta;
                touched_.push_back(id);
            } else {
                // Combine this batch's repeated entity rows: additive for COUNT/SUM/AVG/
                // RATIO, min/max for the extreme functions.
                delta_scratch_[id] = combine_fn(group.function, delta_scratch_[id], delta);
                count_scratch_[id] += count_delta;
            }
        }

        for (int channel_index : group.channel_indices) {
            WindowChannel& ch = channels_[channel_index];
            if (cap > ch.entities.size()) ch.entities.resize(cap);
            for (int32_t id : touched_) {
                EntityCounter& ec = ch.entities[id];
                bool was_init = ec.initialized;
                advance_and_zero(ec, now, ch.duration_seconds, additive);
                if (!was_init) ++ch.active_entities;
                add_bucket(ec, now, delta_scratch_[id], count_scratch_[id], ch.function);
                if (additive) {
                    ec.running_total += delta_scratch_[id];
                    ec.running_count += count_scratch_[id];
                }
            }
        }
    }
}

void WindowStore::evict_stale_entities(int64_t current_time_seconds) {
    for (auto& ch : channels_) {
        for (auto& ec : ch.entities) {
            if (ec.initialized &&
                current_time_seconds - ec.last_write_second > ch.duration_seconds * 2) {
                ec = EntityCounter{};  // free buckets, reset slot (keeps dense index stable)
                if (ch.active_entities > 0) --ch.active_entities;
            }
        }
    }
}

void WindowStore::reset_all() {
    for (auto& ch : channels_) {
        ch.entities.clear();
        ch.active_entities = 0;
    }
}

size_t WindowStore::memory_usage_bytes() const {
    size_t bytes = 0;
    for (const auto& ch : channels_) {
        bytes += ch.entities.capacity() * sizeof(EntityCounter);
        for (const auto& ec : ch.entities) {
            bytes += ec.extra_buckets.size() * (sizeof(int64_t) + sizeof(double));
            bytes += ec.extra_count_buckets.size() * (sizeof(int64_t) + sizeof(double));
        }
    }
    return bytes;
}

int WindowStore::num_active_entities(int channel_index) const {
    return static_cast<int>(channels_[channel_index].active_entities);
}

std::shared_ptr<arrow::RecordBatch> inject_derived_columns(
        const std::shared_ptr<arrow::RecordBatch>& encoded,
        const DerivedColumnPlan& plan,
        const std::vector<const double*>& slot_values) {
    if (plan.slots.empty()) return encoded;
    const int64_t n = encoded->num_rows();

    // Start from the existing fields/columns and append all derived columns once.
    // Slots are in injected_col_index order, which is exactly the append position
    // (num_fields + k), so a single RecordBatch::Make reproduces the per-column
    // AddColumn placement without the O(num_columns) splice per column.
    std::vector<std::shared_ptr<arrow::Field>> fields = encoded->schema()->fields();
    std::vector<std::shared_ptr<arrow::Array>> arrays(encoded->columns().begin(),
                                                      encoded->columns().end());
    fields.reserve(fields.size() + plan.slots.size());
    arrays.reserve(arrays.size() + plan.slots.size());

    for (size_t k = 0; k < plan.slots.size(); ++k) {
        const DerivedColumnSlot& slot = plan.slots[k];
        const double* vals = k < slot_values.size() ? slot_values[k] : nullptr;
        std::shared_ptr<arrow::DataType> dtype;
        std::shared_ptr<arrow::Buffer> buf;
        if (slot.column_type == ColumnType::FLOAT32) {
            dtype = arrow::float32();
            buf = arrow::AllocateBuffer(n * static_cast<int64_t>(sizeof(float))).ValueOrDie();
            float* out = reinterpret_cast<float*>(buf->mutable_data());
            if (vals) {
                for (int64_t i = 0; i < n; ++i) out[i] = static_cast<float>(vals[i]);
            } else {
                std::memset(out, 0, static_cast<size_t>(n) * sizeof(float));
            }
        } else {
            dtype = arrow::int32();
            buf = arrow::AllocateBuffer(n * static_cast<int64_t>(sizeof(int32_t))).ValueOrDie();
            int32_t* out = reinterpret_cast<int32_t*>(buf->mutable_data());
            if (vals) {
                for (int64_t i = 0; i < n; ++i) out[i] = static_cast<int32_t>(vals[i]);
            } else {
                std::memset(out, 0, static_cast<size_t>(n) * sizeof(int32_t));
            }
        }
        auto data = arrow::ArrayData::Make(dtype, n, {nullptr, buf});
        std::string name = slot.injected_name.empty()
            ? "__derived_" + std::to_string(slot.injected_col_index)
            : slot.injected_name;
        fields.push_back(arrow::field(name, dtype, /*nullable=*/false));
        arrays.push_back(arrow::MakeArray(data));
    }
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), n, std::move(arrays));
}
