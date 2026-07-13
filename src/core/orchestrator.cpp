#include "blazerules/orchestrator.h"

#include "blazerules/arena.h"
#include "blazerules/bitmask.h"
#include "blazerules/simd_kernels.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <re2/re2.h>
#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace {

constexpr int MORSEL_ROWS = 2048;
constexpr size_t DEFAULT_ARENA_BYTES = 8u * 1024u * 1024u;

struct ColView {
    const uint8_t* values = nullptr;
    const uint8_t* validity = nullptr;
    const int32_t* offsets32 = nullptr;
    const int64_t* offsets64 = nullptr;
    const uint8_t* string_data = nullptr;
    int64_t offset = 0;
    int64_t validity_offset = 0;
    arrow::Type::type type_id = arrow::Type::NA;
    bool has_nulls = false;
};

struct ZoneMap {
    bool ready = false;
    ColumnType type = ColumnType::INT32;
    double min_value = 0.0;
    double max_value = 0.0;
};

inline bool is_arrow_string_type(arrow::Type::type type_id) {
    return type_id == arrow::Type::STRING || type_id == arrow::Type::LARGE_STRING;
}

struct ThreadScratch {
    explicit ThreadScratch(size_t arena_bytes)
        : arena(std::max(arena_bytes, DEFAULT_ARENA_BYTES)) {}

    BatchArena arena;
    std::vector<uint8_t> predicate_storage;
    std::vector<uint8_t*> predicate_masks;
    std::vector<uint8_t*> node_masks;
    std::vector<int32_t> survivors;
    std::vector<int32_t> next_survivors;
    std::vector<ZoneMap> zones;
};

inline int elem_size(ColumnType t) {
    return (t == ColumnType::FLOAT64 || t == ColumnType::INT64 ||
            t == ColumnType::TIMESTAMP_MS) ? 8 : 4;
}

inline int num_op_index(OpType op) {
    switch (op) {
        case OpType::GT:  return 0;
        case OpType::LT:  return 1;
        case OpType::GTE: return 2;
        case OpType::LTE: return 3;
        case OpType::EQ:  return 4;
        case OpType::NEQ: return 5;
        default:          return 0;
    }
}

inline int cat_op_index(OpType op) {
    switch (op) {
        case OpType::IN:     return 0;
        case OpType::NOT_IN: return 1;
        case OpType::CONTAINS_ANY:
        case OpType::INTERSECTS:
        case OpType::EQ:     return 2;
        case OpType::NOT_INTERSECTS:
        case OpType::NEQ:    return 1;
        default:             return 0;
    }
}

inline const void* numeric_ptr(const std::vector<ColView>& cols, int col_index,
                               ColumnType type, int r0) {
    const ColView& cv = cols[col_index];
    return cv.values + static_cast<size_t>(cv.offset + r0) * elem_size(type);
}

inline const uint8_t* validity_ptr(const std::vector<ColView>& cols, int col_index, int r0) {
    const ColView& cv = cols[col_index];
    return cv.validity ? cv.validity + ((cv.validity_offset + r0) >> 3) : nullptr;
}

inline int bitmask_bytes(int n_records) {
    return (n_records + 7) / 8;
}

inline void and_into(uint8_t* dst, const uint8_t* src, int nbytes) {
    int i = 0;
#if defined(__aarch64__)
    for (; i + 16 <= nbytes; i += 16) {
        uint8x16_t a = vld1q_u8(dst + i);
        uint8x16_t b = vld1q_u8(src + i);
        vst1q_u8(dst + i, vandq_u8(a, b));
    }
#endif
    for (; i < nbytes; ++i) dst[i] &= src[i];
}

#if defined(__aarch64__)
inline uint8_t neon_pack_u64(uint64x2_t a, uint64x2_t b, uint64x2_t c, uint64x2_t d) {
    static const uint64_t W0[2] = {1, 2}, W1[2] = {4, 8}, W2[2] = {16, 32}, W3[2] = {64, 128};
    uint64_t r = vaddvq_u64(vandq_u64(a, vld1q_u64(W0)))
               | vaddvq_u64(vandq_u64(b, vld1q_u64(W1)))
               | vaddvq_u64(vandq_u64(c, vld1q_u64(W2)))
               | vaddvq_u64(vandq_u64(d, vld1q_u64(W3)));
    return static_cast<uint8_t>(r);
}
inline uint8_t neon_pack_u32(uint32x4_t lo, uint32x4_t hi) {
    static const uint32_t WL[4] = {1, 2, 4, 8}, WH[4] = {16, 32, 64, 128};
    uint32_t r = vaddvq_u32(vandq_u32(lo, vld1q_u32(WL))) | vaddvq_u32(vandq_u32(hi, vld1q_u32(WH)));
    return static_cast<uint8_t>(r);
}
enum class MaskMode { ANY, ALL, NONE };
template <MaskMode MODE>
inline uint64x2_t mask_cmp_u64(uint64x2_t v, uint64x2_t mask) {
    uint64x2_t t = vandq_u64(v, mask);
    if constexpr (MODE == MaskMode::ALL) return vceqq_u64(t, mask);
    else if constexpr (MODE == MaskMode::NONE) return vceqzq_u64(t);
    else return veorq_u64(vceqzq_u64(t), vdupq_n_u64(~0ull));
}
template <MaskMode MODE>
inline uint32x4_t mask_cmp_u32(uint32x4_t v, uint32x4_t mask) {
    uint32x4_t t = vandq_u32(v, mask);
    if constexpr (MODE == MaskMode::ALL) return vceqq_u32(t, mask);
    else if constexpr (MODE == MaskMode::NONE) return vceqzq_u32(t);
    else return veorq_u32(vceqzq_u32(t), vdupq_n_u32(~0u));
}
template <MaskMode MODE>
inline void neon_mask_run_u64(const int64_t* d, int nb, uint64_t mask, uint8_t* out) {
    uint64x2_t vm = vdupq_n_u64(mask);
    for (int b = 0; b < nb; ++b) {
        const int64_t* p = d + b * 8;
        out[b] = neon_pack_u64(
            mask_cmp_u64<MODE>(vreinterpretq_u64_s64(vld1q_s64(p)), vm),
            mask_cmp_u64<MODE>(vreinterpretq_u64_s64(vld1q_s64(p + 2)), vm),
            mask_cmp_u64<MODE>(vreinterpretq_u64_s64(vld1q_s64(p + 4)), vm),
            mask_cmp_u64<MODE>(vreinterpretq_u64_s64(vld1q_s64(p + 6)), vm));
    }
}
template <MaskMode MODE>
inline void neon_mask_run_u32(const int32_t* d, int nb, uint32_t mask, uint8_t* out) {
    uint32x4_t vm = vdupq_n_u32(mask);
    for (int b = 0; b < nb; ++b) {
        const int32_t* p = d + b * 8;
        out[b] = neon_pack_u32(mask_cmp_u32<MODE>(vreinterpretq_u32_s32(vld1q_s32(p)), vm),
                               mask_cmp_u32<MODE>(vreinterpretq_u32_s32(vld1q_s32(p + 4)), vm));
    }
}
template <typename RunFn>
inline void neon_mask_dispatch(MaskMode mode, RunFn&& run) {
    if (mode == MaskMode::ALL) run(std::integral_constant<MaskMode, MaskMode::ALL>{});
    else if (mode == MaskMode::NONE) run(std::integral_constant<MaskMode, MaskMode::NONE>{});
    else run(std::integral_constant<MaskMode, MaskMode::ANY>{});
}
#endif

inline bool bit_is_set(const uint8_t* mask, int idx) {
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

inline void set_bit(uint8_t* mask, int idx) {
    mask[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7));
}

inline void fill_true(uint8_t* mask, int n_records) {
    int nbytes = bitmask_bytes(n_records);
    std::memset(mask, 0xff, nbytes);
    int tail = n_records & 7;
    if (tail != 0) mask[nbytes - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
}

inline void collect_set_bits(const uint8_t* mask, int n_records, std::vector<int32_t>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(n_records / 8));
    int nbytes = bitmask_bytes(n_records);
    for (int b = 0; b < nbytes; ++b) {
        uint8_t byte = mask[b];
        while (byte != 0) {
            int bit = blazerules::ctz32(byte);
            int idx = b * 8 + bit;
            if (idx < n_records) out.push_back(idx);
            byte &= byte - 1;
        }
    }
}

void maybe_apply_affinity(bool enabled, int tag) {
    if (!enabled) return;
#if defined(__APPLE__) && defined(__aarch64__)
    thread_local bool applied = false;
    if (applied) return;
    thread_affinity_policy_data_t policy{static_cast<integer_t>((tag % 64) + 1)};
    (void)thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                            reinterpret_cast<thread_policy_t>(&policy), 1);
    applied = true;
#else
    (void)tag;
#endif
}

int numeric_column_index(const KernelOp& op, ColumnType& type) {
    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) {
        type = n->column_type;
        return n->column_index;
    }
    if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) {
        type = r->column_type;
        return r->column_index;
    }
    if (const auto* w = std::get_if<WindowPredicateOp>(&op)) {
        type = w->column_type;
        return w->window_column_index;
    }
    return -1;
}

double value_at(const uint8_t* values, ColumnType type, int row) {
    switch (type) {
        case ColumnType::FLOAT32: return reinterpret_cast<const float*>(values)[row];
        case ColumnType::FLOAT64: return reinterpret_cast<const double*>(values)[row];
        case ColumnType::INT32: return reinterpret_cast<const int32_t*>(values)[row];
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS: return static_cast<double>(reinterpret_cast<const int64_t*>(values)[row]);
        default: return 0.0;
    }
}

void compute_zone(const std::vector<ColView>& cols, int col_index, ColumnType type,
                  int r0, int m, ZoneMap& zone) {
    const ColView& col = cols[col_index];
    const uint8_t* base = col.values + static_cast<size_t>(col.offset + r0) * elem_size(type);
    if (m <= 0) return;
    double mn = value_at(base, type, 0);
    double mx = mn;
    for (int i = 1; i < m; ++i) {
        double v = value_at(base, type, i);
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    zone.ready = true;
    zone.type = type;
    zone.min_value = mn;
    zone.max_value = mx;
}

bool try_zone_numeric(OpType op, double threshold, const ZoneMap& zone,
                      uint8_t* out, int m) {
    if (!zone.ready) return false;
    bool all_false = false;
    bool all_true = false;
    switch (op) {
        case OpType::GT:
            all_false = zone.max_value <= threshold;
            all_true = zone.min_value > threshold;
            break;
        case OpType::GTE:
            all_false = zone.max_value < threshold;
            all_true = zone.min_value >= threshold;
            break;
        case OpType::LT:
            all_false = zone.min_value >= threshold;
            all_true = zone.max_value < threshold;
            break;
        case OpType::LTE:
            all_false = zone.min_value > threshold;
            all_true = zone.max_value <= threshold;
            break;
        case OpType::EQ:
            all_false = threshold < zone.min_value || threshold > zone.max_value;
            break;
        case OpType::NEQ:
            all_false = zone.min_value == threshold && zone.max_value == threshold;
            all_true = threshold < zone.min_value || threshold > zone.max_value;
            break;
        default:
            break;
    }
    if (all_false) {
        std::memset(out, 0, bitmask_bytes(m));
        return true;
    }
    if (all_true) {
        fill_true(out, m);
        return true;
    }
    return false;
}

bool try_zone_range(OpType op, double lower, double upper, const ZoneMap& zone,
                    uint8_t* out, int m) {
    if (!zone.ready) return false;
    bool incl = op == OpType::BETWEEN_INCLUDING;
    bool all_false = incl
        ? (zone.max_value < lower || zone.min_value > upper)
        : (zone.max_value <= lower || zone.min_value >= upper);
    bool all_true = incl
        ? (zone.min_value >= lower && zone.max_value <= upper)
        : (zone.min_value > lower && zone.max_value < upper);
    if (all_false) {
        std::memset(out, 0, bitmask_bytes(m));
        return true;
    }
    if (all_true) {
        fill_true(out, m);
        return true;
    }
    return false;
}

void prefetch_predicate(const KernelOp& op, const std::vector<ColView>& cols, int r0) {
    int col = -1;
    ColumnType type = ColumnType::INT32;
    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) {
        col = n->column_index;
        type = n->column_type;
    } else if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) {
        col = r->column_index;
        type = r->column_type;
    } else if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) {
        col = c->column_index;
        type = cols[col].type_id == arrow::Type::INT64 ? ColumnType::INT64 : ColumnType::INT32;
    } else if (const auto* a = std::get_if<ArrayLenPredicateOp>(&op)) {
        col = a->column_index;
        type = cols[col].type_id == arrow::Type::INT64 ? ColumnType::INT64 : ColumnType::INT32;
    } else if (const auto* z = std::get_if<NullPredicateOp>(&op)) {
        col = z->column_index;
        type = cols[col].type_id == arrow::Type::INT64 ? ColumnType::INT64 : ColumnType::INT32;
    } else if (const auto* w = std::get_if<WindowPredicateOp>(&op)) {
        col = w->window_column_index;
        type = w->column_type;
    }
    if (col >= 0 && cols[col].values) {
        blazerules::prefetch_read(cols[col].values + static_cast<size_t>(cols[col].offset + r0) * elem_size(type));
    }
}

uint64_t requested_bitset(const std::vector<int32_t>& ids) {
    uint64_t bits = 0;
    for (int32_t id : ids) {
        if (id >= 0 && id < 63) bits |= (uint64_t{1} << static_cast<unsigned>(id));
    }
    return bits;
}

void eval_categorical_bitset(const int64_t* masks,
                             const uint8_t* validity,
                             const std::vector<int32_t>& ids,
                             OpType op,
                             uint8_t* out,
                             int n) {
    uint64_t requested = requested_bitset(ids);
    auto match = [&](uint64_t value) {
        switch (op) {
            case OpType::CONTAINS_ALL:
                return (value & requested) == requested;
            case OpType::NOT_IN:
            case OpType::NEQ:
            case OpType::NOT_INTERSECTS:
                return (value & requested) == 0;
            case OpType::IN:
            case OpType::EQ:
            case OpType::CONTAINS_ANY:
            case OpType::INTERSECTS:
            default:
                return (value & requested) != 0;
        }
    };
#if defined(__aarch64__)
    if (!validity) {
        MaskMode mode = op == OpType::CONTAINS_ALL ? MaskMode::ALL
            : (op == OpType::NOT_IN || op == OpType::NEQ || op == OpType::NOT_INTERSECTS)
                ? MaskMode::NONE : MaskMode::ANY;
        int nb = n / 8;
        neon_mask_dispatch(mode, [&](auto tag) {
            neon_mask_run_u64<decltype(tag)::value>(masks, nb, requested, out);
        });
        int rem = n - nb * 8;
        if (rem) {
            uint8_t byte = 0;
            for (int i = 0; i < rem; ++i)
                if (match(static_cast<uint64_t>(masks[nb * 8 + i]))) byte |= static_cast<uint8_t>(1u << i);
            out[nb] = byte;
        }
        return;
    }
#endif
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) {
            if (match(static_cast<uint64_t>(masks[b * 8 + i]))) {
                byte |= static_cast<uint8_t>(1u << i);
            }
        }
        if (validity) byte &= validity[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) {
            if (match(static_cast<uint64_t>(masks[nb * 8 + i]))) {
                byte |= static_cast<uint8_t>(1u << i);
            }
        }
        if (validity) byte &= validity[nb];
        out[nb] = byte;
    }
}

inline bool row_valid(const ColView& col, int relative_row);
std::string_view string_at(const ColView& col, int relative_row);

void eval_array_len_predicate(const ArrayLenPredicateOp& op,
                              const std::vector<ColView>& cols,
                              int r0,
                              int m,
                              uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& cv = cols[op.column_index];
    auto compare_len = [&](int len) {
        switch (op.op_type) {
            case OpType::ARRAY_LEN_GT: return len > op.length;
            case OpType::ARRAY_LEN_LT: return len < op.length;
            case OpType::ARRAY_LEN_EQ: return len == op.length;
            default: return false;
        }
    };

    if (cv.type_id == arrow::Type::INT64) {
        const int64_t* masks = reinterpret_cast<const int64_t*>(cv.values) + (cv.offset + r0);
        for (int i = 0; i < m; ++i) {
            if (cv.validity && !row_valid(cv, r0 + i)) continue;
            int len = blazerules::popcount64(static_cast<uint64_t>(masks[i]));
            if (compare_len(len)) set_bit(out, i);
        }
        return;
    }

    if (cv.type_id == arrow::Type::INT32) {
        const int32_t* ids = reinterpret_cast<const int32_t*>(cv.values) + (cv.offset + r0);
        for (int i = 0; i < m; ++i) {
            if (cv.validity && !row_valid(cv, r0 + i)) continue;
            int len = ids[i] >= 0 ? 1 : 0;
            if (compare_len(len)) set_bit(out, i);
        }
        return;
    }

    if (is_arrow_string_type(cv.type_id) && cv.string_data && (cv.offsets32 || cv.offsets64)) {
        for (int i = 0; i < m; ++i) {
            if (cv.validity && !row_valid(cv, r0 + i)) continue;
            int len = static_cast<int>(string_at(cv, r0 + i).size());
            if (compare_len(len)) set_bit(out, i);
        }
    }
}

void eval_null_predicate(const uint8_t* validity, OpType op, uint8_t* out, int n) {
    int nb = bitmask_bytes(n);
    bool is_null = op == OpType::IS_NULL;
    if (!validity) {
        if (is_null) std::memset(out, 0, nb);
        else fill_true(out, n);
        return;
    }
    if (is_null) {
        for (int b = 0; b < nb; ++b) out[b] = static_cast<uint8_t>(~validity[b]);
        int tail = n & 7;
        if (tail != 0 && nb > 0) out[nb - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
    } else {
        std::memcpy(out, validity, nb);
        int tail = n & 7;
        if (tail != 0 && nb > 0) out[nb - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
    }
}

inline bool row_valid(const ColView& col, int relative_row) {
    if (!col.validity) return true;
    int64_t row = col.validity_offset + relative_row;
    return (col.validity[row >> 3] >> (row & 7)) & 1u;
}

std::string_view string_at(const ColView& col, int relative_row) {
    int64_t row = col.offset + relative_row;
    int64_t begin = 0;
    int64_t end = 0;
    if (col.offsets64) {
        begin = col.offsets64[row];
        end = col.offsets64[row + 1];
    } else {
        const int32_t* offsets = col.offsets32;
        begin = offsets[row];
        end = offsets[row + 1];
    }
    return std::string_view(reinterpret_cast<const char*>(col.string_data + begin),
                            static_cast<size_t>(end - begin));
}

bool numeric_compare(double lhs, double rhs, OpType op) {
    switch (op) {
        case OpType::GT:
        case OpType::GT_FIELD: return lhs > rhs;
        case OpType::LT:
        case OpType::LT_FIELD: return lhs < rhs;
        case OpType::GTE:
        case OpType::GTE_FIELD: return lhs >= rhs;
        case OpType::LTE:
        case OpType::LTE_FIELD: return lhs <= rhs;
        case OpType::EQ:
        case OpType::EQ_FIELD: return lhs == rhs;
        case OpType::NEQ:
        case OpType::NEQ_FIELD: return lhs != rhs;
        default: return false;
    }
}

void eval_cross_field(const CrossFieldPredicateOp& op,
                      const std::vector<ColView>& cols,
                      int r0,
                      int m,
                      uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& left = cols[op.left_column_index];
    const ColView& right = cols[op.right_column_index];
    bool string_eq = op.left_type == ColumnType::STRING && op.right_type == ColumnType::STRING &&
        (op.op_type == OpType::EQ_FIELD || op.op_type == OpType::NEQ_FIELD);
    bool categorical_eq = !is_numeric(op.left_type) && !is_numeric(op.right_type) &&
        !string_eq && (op.op_type == OpType::EQ_FIELD || op.op_type == OpType::NEQ_FIELD);
    for (int i = 0; i < m; ++i) {
        if (!row_valid(left, r0 + i) || !row_valid(right, r0 + i)) continue;
        bool matched = false;
        if (string_eq) {
            matched = string_at(left, r0 + i) == string_at(right, r0 + i);
            if (op.op_type == OpType::NEQ_FIELD) matched = !matched;
        } else if (categorical_eq) {
            const uint8_t* left_values = left.values +
                static_cast<size_t>(left.offset + r0) * elem_size(op.left_type);
            const uint8_t* right_values = right.values +
                static_cast<size_t>(right.offset + r0) * elem_size(op.right_type);
            if (left.type_id == arrow::Type::INT64 || right.type_id == arrow::Type::INT64) {
                int64_t l = reinterpret_cast<const int64_t*>(left_values)[i];
                int64_t r = reinterpret_cast<const int64_t*>(right_values)[i];
                matched = (l == r);
            } else {
                int32_t l = reinterpret_cast<const int32_t*>(left_values)[i];
                int32_t r = reinterpret_cast<const int32_t*>(right_values)[i];
                matched = (l == r);
            }
            if (op.op_type == OpType::NEQ_FIELD) matched = !matched;
        } else {
            const uint8_t* left_values = left.values +
                static_cast<size_t>(left.offset + r0) * elem_size(op.left_type);
            const uint8_t* right_values = right.values +
                static_cast<size_t>(right.offset + r0) * elem_size(op.right_type);
            matched = numeric_compare(value_at(left_values, op.left_type, i),
                                      value_at(right_values, op.right_type, i),
                                      op.op_type);
        }
        if (matched) set_bit(out, i);
    }
}

void eval_bitfield(const BitfieldPredicateOp& op,
                   const std::vector<ColView>& cols,
                   int r0,
                   int m,
                   uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& col = cols[op.column_index];
    const uint8_t* values = col.values + static_cast<size_t>(col.offset + r0) * elem_size(op.column_type);
    auto match_scalar = [&](uint64_t value) {
        switch (op.op_type) {
            case OpType::FLAGS_ALL: return (value & op.mask) == op.mask;
            case OpType::FLAGS_NONE: return (value & op.mask) == 0;
            default: return (value & op.mask) != 0;
        }
    };
#if defined(__aarch64__)
    if (!col.validity) {
        MaskMode mode = op.op_type == OpType::FLAGS_ALL ? MaskMode::ALL
                      : op.op_type == OpType::FLAGS_NONE ? MaskMode::NONE
                                                         : MaskMode::ANY;
        int nb = m / 8;
        if (op.column_type == ColumnType::INT64) {
            const int64_t* d = reinterpret_cast<const int64_t*>(values);
            neon_mask_dispatch(mode, [&](auto tag) {
                neon_mask_run_u64<decltype(tag)::value>(d, nb, op.mask, out);
            });
        } else {
            const int32_t* d = reinterpret_cast<const int32_t*>(values);
            neon_mask_dispatch(mode, [&](auto tag) {
                neon_mask_run_u32<decltype(tag)::value>(d, nb, static_cast<uint32_t>(op.mask), out);
            });
        }
        for (int i = nb * 8; i < m; ++i) {
            uint64_t value = op.column_type == ColumnType::INT32
                ? static_cast<uint64_t>(reinterpret_cast<const int32_t*>(values)[i])
                : static_cast<uint64_t>(reinterpret_cast<const int64_t*>(values)[i]);
            if (match_scalar(value)) set_bit(out, i);
        }
        return;
    }
#endif
    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        uint64_t value = op.column_type == ColumnType::INT32
            ? static_cast<uint64_t>(reinterpret_cast<const int32_t*>(values)[i])
            : static_cast<uint64_t>(reinterpret_cast<const int64_t*>(values)[i]);
        if (match_scalar(value)) set_bit(out, i);
    }
}

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool ci_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    }
    return true;
}

void eval_string_predicate(const StringPredicateOp& op,
                           const std::vector<ColView>& cols,
                           int r0,
                           int m,
                           uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& col = cols[op.column_index];
    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        std::string_view value = string_at(col, r0 + i);
        bool matched = false;
        switch (op.op_type) {
            case OpType::CONTAINS:
                matched = value.find(op.pattern) != std::string_view::npos;
                break;
            case OpType::STARTS_WITH:
                matched = value.size() >= op.pattern.size() &&
                    value.substr(0, op.pattern.size()) == op.pattern;
                break;
            case OpType::ENDS_WITH:
                matched = value.size() >= op.pattern.size() &&
                    value.substr(value.size() - op.pattern.size()) == op.pattern;
                break;
            case OpType::CI_EQ:
                matched = ci_equal(value, op.pattern);
                break;
            case OpType::LENGTH_GT:
                matched = static_cast<int>(value.size()) > op.length;
                break;
            case OpType::LENGTH_LT:
                matched = static_cast<int>(value.size()) < op.length;
                break;
            case OpType::LENGTH_EQ:
                matched = static_cast<int>(value.size()) == op.length;
                break;
            default:
                break;
        }
        if (matched) set_bit(out, i);
    }
}

bool sorted_string_contains(const std::vector<std::string>& values, std::string_view needle) {
    auto it = std::lower_bound(values.begin(), values.end(), needle,
        [](const std::string& lhs, std::string_view rhs) {
            return std::string_view(lhs) < rhs;
        });
    return it != values.end() && std::string_view(*it) == needle;
}

bool sorted_int_contains(const std::vector<int64_t>& values, int64_t needle) {
    return std::binary_search(values.begin(), values.end(), needle);
}

bool ipv4_ranges_contain(const std::vector<Ipv4Range>& ranges, uint32_t ip) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), ip,
        [](uint32_t value, const Ipv4Range& range) {
            return value < range.start;
        });
    if (it == ranges.begin()) return false;
    --it;
    return ip >= it->start && ip <= it->end;
}

void eval_regex_predicate(const RegexPredicateOp& op,
                          const std::vector<ColView>& cols,
                          int r0,
                          int m,
                          uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    if (!op.regex) return;
    const ColView& col = cols[op.column_index];
    bool negate = op.op_type == OpType::NOT_REGEX;
    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        std::string_view value = string_at(col, r0 + i);
        re2::StringPiece piece(value.data(), static_cast<int>(value.size()));
        bool matched = RE2::PartialMatch(piece, *op.regex);
        if (negate) matched = !matched;
        if (matched) set_bit(out, i);
    }
}

uint32_t parse_ipv4_fast(std::string_view s) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    uint32_t value = 0;
    bool saw_digit = false;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10u + static_cast<uint32_t>(ch - '0');
            saw_digit = true;
        } else if (ch == '.' && part < 3) {
            parts[part++] = value;
            value = 0;
            saw_digit = false;
        } else {
            return 0;
        }
    }
    if (!saw_digit || part != 3) return 0;
    parts[part] = value;
    return (parts[0] << 24u) | (parts[1] << 16u) | (parts[2] << 8u) | parts[3];
}

void eval_lookup_predicate(const LookupPredicateOp& op,
                           const std::vector<ColView>& cols,
                           const std::vector<int32_t>& resolved_ids,
                           int r0,
                           int m,
                           uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    if (!op.lookup) return;
    const ColView& col = cols[op.column_index];
    bool negate = op.op_type == OpType::NOT_IN_LOOKUP;

    if (op.lookup->type == LookupSetType::STRING_SET &&
        (op.column_type == ColumnType::CATEGORICAL || op.column_type == ColumnType::ENTITY_KEY)) {
        if (col.type_id == arrow::Type::INT64) {
            uint64_t requested = requested_bitset(resolved_ids);
            const int64_t* masks = reinterpret_cast<const int64_t*>(col.values) + (col.offset + r0);
            for (int i = 0; i < m; ++i) {
                if (!row_valid(col, r0 + i)) continue;
                bool matched = (static_cast<uint64_t>(masks[i]) & requested) != 0;
                if (negate) matched = !matched;
                if (matched) set_bit(out, i);
            }
        } else {
            const int32_t* values = reinterpret_cast<const int32_t*>(col.values) + (col.offset + r0);
            for (int i = 0; i < m; ++i) {
                if (!row_valid(col, r0 + i)) continue;
                bool matched = std::binary_search(resolved_ids.begin(), resolved_ids.end(), values[i]);
                if (negate) matched = !matched;
                if (matched) set_bit(out, i);
            }
        }
        return;
    }

    if (op.lookup->type == LookupSetType::STRING_SET) {
        for (int i = 0; i < m; ++i) {
            if (!row_valid(col, r0 + i)) continue;
            bool matched = sorted_string_contains(op.lookup->strings, string_at(col, r0 + i));
            if (negate) matched = !matched;
            if (matched) set_bit(out, i);
        }
        return;
    }

    if (op.lookup->type == LookupSetType::INT_SET) {
        const uint8_t* values = col.values + static_cast<size_t>(col.offset + r0) * elem_size(op.column_type);
        for (int i = 0; i < m; ++i) {
            if (!row_valid(col, r0 + i)) continue;
            int64_t value = static_cast<int64_t>(value_at(values, op.column_type, i));
            bool matched = sorted_int_contains(op.lookup->ints, value);
            if (negate) matched = !matched;
            if (matched) set_bit(out, i);
        }
        return;
    }

    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        uint32_t ip = 0;
        if (is_arrow_string_type(col.type_id)) {
            ip = parse_ipv4_fast(string_at(col, r0 + i));
        } else if (op.column_type == ColumnType::INT32) {
            const auto* values = reinterpret_cast<const int32_t*>(
                col.values + static_cast<size_t>(col.offset + r0) * sizeof(int32_t));
            ip = static_cast<uint32_t>(values[i]);
        } else {
            const auto* values = reinterpret_cast<const int64_t*>(
                col.values + static_cast<size_t>(col.offset + r0) * sizeof(int64_t));
            ip = static_cast<uint32_t>(values[i]);
        }
        bool matched = ipv4_ranges_contain(op.lookup->ipv4_ranges, ip);
        if (negate) matched = !matched;
        if (matched) set_bit(out, i);
    }
}

void eval_cidr_predicate(const CidrPredicateOp& op,
                         const std::vector<ColView>& cols,
                         int r0,
                         int m,
                         uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& col = cols[op.column_index];
    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        uint32_t ip = 0;
        if (is_arrow_string_type(col.type_id)) {
            ip = parse_ipv4_fast(string_at(col, r0 + i));
        } else if (op.column_type == ColumnType::INT32) {
            const auto* values = reinterpret_cast<const int32_t*>(
                col.values + static_cast<size_t>(col.offset + r0) * sizeof(int32_t));
            ip = static_cast<uint32_t>(values[i]);
        } else {
            const auto* values = reinterpret_cast<const int64_t*>(
                col.values + static_cast<size_t>(col.offset + r0) * sizeof(int64_t));
            ip = static_cast<uint32_t>(values[i]);
        }
        bool matched = (ip & op.mask) == op.network;
        if (op.op_type == OpType::IP_NOT_IN_SUBNET) matched = !matched;
        if (matched) set_bit(out, i);
    }
}

void eval_temporal_predicate(const TemporalPredicateOp& op,
                             const std::vector<ColView>& cols,
                             int r0,
                             int m,
                             uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& col = cols[op.column_index];
    const auto* ts = reinterpret_cast<const int64_t*>(
        col.values + static_cast<size_t>(col.offset + r0) * sizeof(int64_t));
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int i = 0; i < m; ++i) {
        if (!row_valid(col, r0 + i)) continue;
        int64_t v = ts[i];
        bool matched = false;
        switch (op.op_type) {
            case OpType::BEFORE:
                matched = v < op.value;
                break;
            case OpType::AFTER:
                matched = v > op.value;
                break;
            case OpType::WITHIN_LAST:
                matched = v >= now_ms - op.value * 1000;
                break;
            case OpType::DAY_OF_WEEK_IN: {
                int64_t days = v / 86400000LL;
                int dow = static_cast<int>((days + 4) % 7);
                if (dow < 0) dow += 7;
                matched = std::find(op.values.begin(), op.values.end(), dow) != op.values.end();
                break;
            }
            case OpType::TIME_OF_DAY_BETWEEN: {
                int64_t ms_day = v % 86400000LL;
                if (ms_day < 0) ms_day += 86400000LL;
                double hour = static_cast<double>(ms_day) / 3600000.0;
                matched = op.lower <= op.upper
                    ? (hour >= op.lower && hour <= op.upper)
                    : (hour >= op.lower || hour <= op.upper);
                break;
            }
            default:
                break;
        }
        if (matched) set_bit(out, i);
    }
}

double eval_value_node(const std::vector<ValueExprNode>& nodes,
                       int idx,
                       const std::vector<ColView>& cols,
                       int absolute_row,
                       bool& valid) {
    const ValueExprNode& node = nodes[idx];
    switch (node.kind) {
        case ValueExprKind::FIELD: {
            const ColView& col = cols[node.column_index];
            if (!row_valid(col, absolute_row)) {
                valid = false;
                return 0.0;
            }
            const uint8_t* base = col.values +
                static_cast<size_t>(col.offset) * elem_size(node.column_type);
            return value_at(base, node.column_type, absolute_row);
        }
        case ValueExprKind::LITERAL:
            return node.literal;
        case ValueExprKind::ADD:
            return eval_value_node(nodes, node.left, cols, absolute_row, valid) +
                   eval_value_node(nodes, node.right, cols, absolute_row, valid);
        case ValueExprKind::SUB:
            return eval_value_node(nodes, node.left, cols, absolute_row, valid) -
                   eval_value_node(nodes, node.right, cols, absolute_row, valid);
        case ValueExprKind::MUL:
            return eval_value_node(nodes, node.left, cols, absolute_row, valid) *
                   eval_value_node(nodes, node.right, cols, absolute_row, valid);
        case ValueExprKind::DIV: {
            double rhs = eval_value_node(nodes, node.right, cols, absolute_row, valid);
            double lhs = eval_value_node(nodes, node.left, cols, absolute_row, valid);
            if (rhs == 0.0) {
                valid = false;
                return 0.0;
            }
            return lhs / rhs;
        }
    }
    valid = false;
    return 0.0;
}

void eval_arithmetic_predicate(const ArithmeticPredicateOp& op,
                               const std::vector<ColView>& cols,
                               int r0,
                               int m,
                               uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    for (int i = 0; i < m; ++i) {
        bool valid = true;
        double lhs = eval_value_node(op.value_nodes, op.root_value, cols, r0 + i, valid);
        if (!valid) continue;
        double rhs = op.threshold;
        if (op.other_column_index >= 0) {
            const ColView& other = cols[op.other_column_index];
            if (!row_valid(other, r0 + i)) continue;
            const uint8_t* base = other.values +
                static_cast<size_t>(other.offset) * elem_size(op.other_column_type);
            rhs = value_at(base, op.other_column_type, r0 + i);
        }
        if (numeric_compare(lhs, rhs, op.op_type)) set_bit(out, i);
    }
}

double deg_to_rad(double v) {
    return v * 0.01745329251994329576923690768489;
}

void eval_geo_distance_predicate(const GeoDistancePredicateOp& op,
                                 const std::vector<ColView>& cols,
                                 int r0,
                                 int m,
                                 uint8_t* out) {
    std::memset(out, 0, bitmask_bytes(m));
    const ColView& lat = cols[op.lat_column_index];
    const ColView& lon = cols[op.lon_column_index];
    const ColView& other_lat = cols[op.other_lat_column_index];
    const ColView& other_lon = cols[op.other_lon_column_index];
    auto value = [&](const ColView& col, int row) {
        ColumnType type = col.type_id == arrow::Type::DOUBLE ? ColumnType::FLOAT64 : ColumnType::FLOAT32;
        const uint8_t* base = col.values + static_cast<size_t>(col.offset) * elem_size(type);
        return value_at(base, type, row);
    };
    for (int i = 0; i < m; ++i) {
        int row = r0 + i;
        if (!row_valid(lat, row) || !row_valid(lon, row) ||
            !row_valid(other_lat, row) || !row_valid(other_lon, row)) {
            continue;
        }
        double lat1 = deg_to_rad(value(lat, row));
        double lon1 = deg_to_rad(value(lon, row));
        double lat2 = deg_to_rad(value(other_lat, row));
        double lon2 = deg_to_rad(value(other_lon, row));
        double dlat = lat2 - lat1;
        double dlon = lon2 - lon1;
        double x = dlon * std::cos((lat1 + lat2) * 0.5);
        double y = dlat;
        double dist_km = 6371.0088 * std::sqrt(x * x + y * y);
        if (dist_km > 1500.0) {
            double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                       std::cos(lat1) * std::cos(lat2) *
                       std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
            dist_km = 6371.0088 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        }
        bool matched = op.op_type == OpType::DISTANCE_LT
            ? dist_km < op.threshold_km
            : dist_km > op.threshold_km;
        if (matched) set_bit(out, i);
    }
}

void eval_categorical_scalar(const int32_t* values,
                             const uint8_t* validity,
                             const std::vector<int32_t>& ids,
                             OpType op,
                             uint8_t* out,
                             int n) {
    std::memset(out, 0, bitmask_bytes(n));
    bool positive = op == OpType::IN || op == OpType::EQ ||
                    op == OpType::CONTAINS_ANY || op == OpType::INTERSECTS;
    bool contains_all = op == OpType::CONTAINS_ALL;
    for (int i = 0; i < n; ++i) {
        if (validity && ((validity[i >> 3] >> (i & 7)) & 1u) == 0) continue;
        bool found = false;
        for (int32_t id : ids) {
            if (values[i] == id) {
                found = true;
                break;
            }
        }
        bool matched = contains_all ? (ids.size() <= 1 && found) : (positive ? found : !found);
        if (matched) set_bit(out, i);
    }
}

void eval_predicate(const KernelOp& op,
                    const std::vector<ColView>& cols,
                    const KernelDispatchTable& disp,
                    const std::vector<int32_t>& resolved_ids,
                    int r0,
                    int m,
                    bool enable_prefetch,
                    const std::vector<ZoneMap>& zones,
                    uint8_t* out) {
    if (enable_prefetch) prefetch_predicate(op, cols, r0);

    if (const auto* n = std::get_if<NumericPredicateOp>(&op)) {
        if (try_zone_numeric(n->op_type, n->threshold, zones[n->column_index], out, m)) return;
        disp.numeric[numeric_type_index(n->column_type)][num_op_index(n->op_type)](
            numeric_ptr(cols, n->column_index, n->column_type, r0),
            validity_ptr(cols, n->column_index, r0),
            n->threshold, out, m);
    } else if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) {
        if (try_zone_range(r->op_type, r->lower, r->upper, zones[r->column_index], out, m)) return;
        int ri = (r->op_type == OpType::BETWEEN_INCLUDING) ? 0 : 1;
        disp.range[numeric_type_index(r->column_type)][ri](
            numeric_ptr(cols, r->column_index, r->column_type, r0),
            validity_ptr(cols, r->column_index, r0),
            r->lower, r->upper, out, m);
    } else if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) {
        const ColView& cv = cols[c->column_index];
        if (cv.type_id == arrow::Type::INT64) {
            const int64_t* masks = reinterpret_cast<const int64_t*>(cv.values) + (cv.offset + r0);
            eval_categorical_bitset(masks, validity_ptr(cols, c->column_index, r0),
                                    resolved_ids, c->op_type, out, m);
        } else {
            const int32_t* d = reinterpret_cast<const int32_t*>(cv.values) + (cv.offset + r0);
            if (c->op_type == OpType::CONTAINS_ALL || c->op_type == OpType::CONTAINS_ANY ||
                c->op_type == OpType::INTERSECTS || c->op_type == OpType::NOT_INTERSECTS) {
                eval_categorical_scalar(d, validity_ptr(cols, c->column_index, r0),
                                        resolved_ids, c->op_type, out, m);
            } else {
                disp.string_[cat_op_index(c->op_type)](
                    d, validity_ptr(cols, c->column_index, r0),
                    resolved_ids.data(), static_cast<int>(resolved_ids.size()), out, m);
            }
        }
    } else if (const auto* a = std::get_if<ArrayLenPredicateOp>(&op)) {
        eval_array_len_predicate(*a, cols, r0, m, out);
    } else if (const auto* z = std::get_if<NullPredicateOp>(&op)) {
        if (z->op_type == OpType::IS_EMPTY || z->op_type == OpType::IS_NOT_EMPTY) {
            const ColView& cv = cols[z->column_index];
            if (is_arrow_string_type(cv.type_id) && cv.string_data && (cv.offsets32 || cv.offsets64)) {
                StringPredicateOp pseudo;
                pseudo.column_index = z->column_index;
                pseudo.op_type = z->op_type == OpType::IS_EMPTY ? OpType::LENGTH_EQ : OpType::LENGTH_GT;
                pseudo.length = 0;
                eval_string_predicate(pseudo, cols, r0, m, out);
            } else {
                eval_null_predicate(validity_ptr(cols, z->column_index, r0),
                                    z->op_type == OpType::IS_EMPTY ? OpType::IS_NULL : OpType::IS_NOT_NULL,
                                    out, m);
            }
            if (z->op_type == OpType::IS_EMPTY && cols[z->column_index].validity) {
                uint8_t nulls[512];
                if (m <= static_cast<int>(sizeof(nulls) * 8)) {
                    eval_null_predicate(validity_ptr(cols, z->column_index, r0), OpType::IS_NULL, nulls, m);
                    blazerules::or_into(out, nulls, bitmask_bytes(m));
                }
            }
        } else {
            eval_null_predicate(validity_ptr(cols, z->column_index, r0), z->op_type, out, m);
        }
    } else if (const auto* cf = std::get_if<CrossFieldPredicateOp>(&op)) {
        eval_cross_field(*cf, cols, r0, m, out);
    } else if (const auto* b = std::get_if<BitfieldPredicateOp>(&op)) {
        eval_bitfield(*b, cols, r0, m, out);
    } else if (const auto* s = std::get_if<StringPredicateOp>(&op)) {
        eval_string_predicate(*s, cols, r0, m, out);
    } else if (const auto* r = std::get_if<RegexPredicateOp>(&op)) {
        eval_regex_predicate(*r, cols, r0, m, out);
    } else if (const auto* l = std::get_if<LookupPredicateOp>(&op)) {
        eval_lookup_predicate(*l, cols, resolved_ids, r0, m, out);
    } else if (const auto* cidr = std::get_if<CidrPredicateOp>(&op)) {
        eval_cidr_predicate(*cidr, cols, r0, m, out);
    } else if (const auto* t = std::get_if<TemporalPredicateOp>(&op)) {
        eval_temporal_predicate(*t, cols, r0, m, out);
    } else if (const auto* g = std::get_if<GeoDistancePredicateOp>(&op)) {
        eval_geo_distance_predicate(*g, cols, r0, m, out);
    } else if (const auto* a = std::get_if<ArithmeticPredicateOp>(&op)) {
        eval_arithmetic_predicate(*a, cols, r0, m, out);
    } else if (const auto* w = std::get_if<WindowPredicateOp>(&op)) {
        if (try_zone_numeric(w->op_type, w->threshold, zones[w->window_column_index], out, m)) return;
        disp.numeric[numeric_type_index(w->column_type)][num_op_index(w->op_type)](
            numeric_ptr(cols, w->window_column_index, w->column_type, r0),
            validity_ptr(cols, w->window_column_index, r0),
            w->threshold, out, m);
    }
}

struct ExprEvaluator {
    const RuleEvalPlan& plan;
    const std::vector<uint8_t*>& predicates;
    const KernelDispatchTable& disp;
    const EvalOptions& options;
    BatchArena& arena;
    std::vector<uint8_t*>& node_masks;
    std::vector<int32_t>& survivors;
    std::vector<int32_t>& next_survivors;
    int m;

    uint8_t* alloc_mask() const {
        uint8_t* mask = arena.allocate_bitmask(m);
        return mask;
    }

    uint8_t* eval(int node_idx) {
        uint8_t*& cached = node_masks[node_idx];
        if (cached) return cached;

        const RuleExprNode& node = plan.nodes[node_idx];
        if (node.kind == RuleExprKind::PREDICATE) {
            cached = predicates[node.predicate_index];
            return cached;
        }

        int mb = bitmask_bytes(m);
        uint8_t* out = alloc_mask();
        if (node.kind == RuleExprKind::NOT) {
            const uint8_t* child = eval(node.children.front());
            disp.bitwise_not(child, out, mb, m);
            cached = out;
            return cached;
        }

        const std::vector<int>& children =
            (node.kind == RuleExprKind::AND &&
             !options.enable_adaptive_predicate_ordering &&
             !node.original_children.empty())
                ? node.original_children
                : node.children;

        if (children.empty()) {
            cached = out;
            return cached;
        }

        const uint8_t* first = eval(children.front());
        std::memcpy(out, first, mb);

        if (node.kind == RuleExprKind::OR) {
            for (size_t i = 1; i < children.size(); ++i) {
                blazerules::or_into(out, eval(children[i]), mb);
            }
            cached = out;
            return cached;
        }

        int first_count = options.enable_selection_vectors
            ? blazerules::count_set_bits(first, mb) : m;
        int threshold = static_cast<int>(options.selection_vector_threshold * static_cast<double>(m));
        if (options.enable_selection_vectors && children.size() > 1 &&
            first_count <= threshold) {
            std::memset(out, 0, mb);
            if (first_count == 0) {
                cached = out;
                return cached;
            }
            collect_set_bits(first, m, survivors);
            for (size_t i = 1; i < children.size() && !survivors.empty(); ++i) {
                const uint8_t* child_mask = eval(children[i]);
                next_survivors.clear();
                next_survivors.reserve(survivors.size());
                for (int32_t idx : survivors) {
                    if (bit_is_set(child_mask, idx)) next_survivors.push_back(idx);
                }
                survivors.swap(next_survivors);
            }
            for (int32_t idx : survivors) set_bit(out, idx);
            cached = out;
            return cached;
        }

        for (size_t i = 1; i < children.size(); ++i) {
            and_into(out, eval(children[i]), mb);
        }
        cached = out;
        return cached;
    }
};

void allocate_rule_bitmasks(const CompiledRuleSet& rs, int n, BatchResult& out,
                            std::vector<uint8_t*>& out_ptr) {
    int bb = bitmask_bytes(n);
    out_ptr.assign(rs.rules.size(), nullptr);
    for (size_t r = 0; r < rs.rules.size(); ++r) {
        auto& v = out.rule_bitmasks[rs.rules[r].rule_id];
        v.assign(bb, 0);
        out_ptr[r] = v.data();
    }
}

int action_rank(ActionType action) {
    switch (action) {
        case ActionType::BLOCK: return 4;
        case ActionType::REVIEW: return 3;
        case ActionType::FLAG: return 2;
        case ActionType::SCORE: return 1;
        case ActionType::APPROVE: return 0;
    }
    return 0;
}

int severity_rank(RuleSeverity severity) {
    switch (severity) {
        case RuleSeverity::CRITICAL: return 3;
        case RuleSeverity::HIGH: return 2;
        case RuleSeverity::MEDIUM: return 1;
        case RuleSeverity::LOW: return 0;
    }
    return 0;
}

const char* risk_band_for(double score, std::string_view decision) {
    if (decision == "BLOCK" || score >= 80.0) return "HIGH";
    if (decision == "REVIEW" || score >= 40.0) return "MEDIUM";
    if (decision == "FLAG" || score > 0.0) return "LOW";
    return "LOW";
}

struct RuleReduce {
    int32_t action_rank = 0;
    int32_t action_code = 0;
    int8_t severity_rank = -1;
    int32_t priority = 0;
    double contribution = 0.0;
    double score_cap = 0.0;
    bool shadow = false;
};

} // namespace

void evaluate_rules(const arrow::RecordBatch& batch,
                    const CompiledRuleSet& rs,
                    const ResolvedKernelBindings& resolved,
                    const EvalOptions& options,
                    BatchResult& out) {
    int n = static_cast<int>(batch.num_rows());
    out.n_records = n;
    out.rule_set_version = rs.version;
    out.decision_labels = rs.decision_labels;
    out.rule_match_counts.clear();
    out.rule_bitmasks.clear();
    out.grouped_decision_indices.clear();
    out.grouped_winning_rule_indices.clear();
    out.matched_record_indices.clear();
    out.decisions.clear();
    out.decision_codes.clear();
    out.scores.clear();
    out.risk_bands.clear();
    out.winning_rule_ids.clear();
    out.n_matched = 0;
    if (n == 0 || rs.rules.empty()) {
        return;
    }

    std::vector<ColView> cols(batch.num_columns());
    std::vector<std::vector<uint8_t>> normalized_validities;
    normalized_validities.reserve(static_cast<size_t>(batch.num_columns()));
    for (int c = 0; c < batch.num_columns(); ++c) {
        const auto& ad = batch.column(c)->data();
        cols[c].values = ad->buffers.size() > 1 && ad->buffers[1] ? ad->buffers[1]->data() : nullptr;
        bool no_nulls = options.enable_no_validity_fast_path && ad->GetNullCount() == 0;
        const uint8_t* raw_validity = (!no_nulls && !ad->buffers.empty() && ad->buffers[0])
            ? ad->buffers[0]->data() : nullptr;
        cols[c].validity = raw_validity;
        cols[c].validity_offset = ad->offset;
        if (raw_validity && ((ad->offset & 7) != 0)) {
            auto& normalized = normalized_validities.emplace_back(static_cast<size_t>(bitmask_bytes(n)), 0);
            for (int row = 0; row < n; ++row) {
                int64_t source_row = ad->offset + row;
                if ((raw_validity[source_row >> 3] >> (source_row & 7)) & 1u) {
                    set_bit(normalized.data(), row);
                }
            }
            cols[c].validity = normalized.data();
            cols[c].validity_offset = 0;
        }
        if (ad->buffers.size() > 1 && ad->buffers[1]) {
            if (batch.column(c)->type_id() == arrow::Type::LARGE_STRING) {
                cols[c].offsets64 = reinterpret_cast<const int64_t*>(ad->buffers[1]->data());
            } else {
                cols[c].offsets32 = reinterpret_cast<const int32_t*>(ad->buffers[1]->data());
            }
        }
        cols[c].string_data = ad->buffers.size() > 2 && ad->buffers[2]
            ? ad->buffers[2]->data()
            : nullptr;
        cols[c].offset = ad->offset;
        cols[c].type_id = batch.column(c)->type_id();
        cols[c].has_nulls = ad->GetNullCount() > 0;
    }

    int bb = bitmask_bytes(n);
    int R = static_cast<int>(rs.rules.size());

    const bool reduce_decisions =
        options.materialize_decision_codes ||
        options.materialize_decision_strings ||
        options.materialize_scores ||
        options.materialize_risk_bands ||
        options.materialize_winning_rules ||
        options.materialize_grouped_indices;
    const bool need_codes =
        options.materialize_decision_codes ||
        options.materialize_decision_strings ||
        options.materialize_risk_bands ||
        options.materialize_grouped_indices;
    const bool need_scores =
        options.materialize_scores ||
        options.materialize_risk_bands;
    const bool need_winning =
        options.materialize_winning_rules ||
        options.materialize_grouped_indices;

    int default_code = 0;
    auto default_it = rs.decision_label_to_code.find(rs.default_decision);
    if (default_it != rs.decision_label_to_code.end()) default_code = default_it->second;
    std::string default_label = default_code >= 0 && default_code < static_cast<int>(rs.decision_labels.size())
        ? rs.decision_labels[static_cast<size_t>(default_code)]
        : std::string("APPROVE");

    std::vector<RuleReduce> reduce;
    std::vector<int32_t> best_action;
    std::vector<int8_t> best_severity;
    std::vector<int32_t> best_priority;
    std::vector<int32_t> winning_idx;
    if (reduce_decisions) {
        reduce.resize(static_cast<size_t>(R));
        for (int r = 0; r < R; ++r) {
            const auto& rule = rs.rules[r];
            reduce[r].action_rank = rule.action_rank > 0
                ? rule.action_rank
                : action_rank(rule.action);
            reduce[r].action_code = rule.action_code;
            reduce[r].severity_rank = static_cast<int8_t>(severity_rank(rule.severity));
            reduce[r].priority = rule.priority;
            double contribution = rule.weight;
            if (contribution == 0.0 && rule.action != ActionType::APPROVE) {
                contribution = static_cast<double>((severity_rank(rule.severity) + 1) * 10);
            }
            reduce[r].contribution = contribution;
            reduce[r].score_cap = rule.score_cap;
            reduce[r].shadow = rule.shadow;
        }
        if (need_scores) out.scores.assign(n, 0.0);
        if (options.materialize_decision_strings) out.decisions.assign(n, default_label);
        if (need_codes) out.decision_codes.assign(n, default_code);
        if (options.materialize_risk_bands) out.risk_bands.assign(n, "LOW");
        if (options.materialize_winning_rules) out.winning_rule_ids.assign(n, "");
        best_action.assign(static_cast<size_t>(n),
            default_code >= 0 && default_code < static_cast<int>(rs.decision_ranks.size())
                ? rs.decision_ranks[static_cast<size_t>(default_code)]
                : 0);
        best_severity.assign(static_cast<size_t>(n), -1);
        best_priority.assign(static_cast<size_t>(n), std::numeric_limits<int>::min());
        if (need_winning) winning_idx.assign(static_cast<size_t>(n), -1);
    }
    std::vector<uint8_t> uni(static_cast<size_t>(bb), 0);

    const bool materialize = options.materialize_rule_bitmasks;
    std::vector<uint8_t*> out_ptr;
    if (materialize) allocate_rule_bitmasks(rs, n, out, out_ptr);

    const KernelDispatchTable& disp = kernels();
    const GlobalPredicatePlan& plan = rs.global_plan;
    int predicate_count = static_cast<int>(plan.predicates.size());
    std::vector<int> numeric_predicate_counts(batch.num_columns(), 0);
    std::vector<ColumnType> numeric_predicate_types(batch.num_columns(), ColumnType::INT32);
    for (const KernelOp& op : plan.predicates) {
        ColumnType type = ColumnType::INT32;
        int col = numeric_column_index(op, type);
        if (col >= 0 && col < batch.num_columns()) {
            ++numeric_predicate_counts[col];
            numeric_predicate_types[col] = type;
        }
    }

    tbb::enumerable_thread_specific<ThreadScratch> scratch_pool(
        [&] { return ThreadScratch(options.arena_size_bytes); });
    tbb::enumerable_thread_specific<std::vector<int64_t>> count_pool(
        [&] { return std::vector<int64_t>(static_cast<size_t>(R), 0); });

    auto do_morsel = [&](int mi) {
        maybe_apply_affinity(options.enable_thread_affinity, mi);

        int r0 = mi * MORSEL_ROWS;
        int m = std::min(MORSEL_ROWS, n - r0);
        int mb = bitmask_bytes(m);
        ThreadScratch& scratch = scratch_pool.local();

        scratch.predicate_storage.assign(static_cast<size_t>(predicate_count) * mb, 0);
        scratch.predicate_masks.resize(predicate_count);
        for (int p = 0; p < predicate_count; ++p) {
            scratch.predicate_masks[p] = scratch.predicate_storage.data() + static_cast<size_t>(p) * mb;
        }

        scratch.zones.assign(batch.num_columns(), ZoneMap{});
        for (int c = 0; c < batch.num_columns(); ++c) {
            if (numeric_predicate_counts[c] > 1 && !cols[c].has_nulls) {
                compute_zone(cols, c, numeric_predicate_types[c], r0, m, scratch.zones[c]);
            }
        }

        for (int predicate_id : plan.predicate_eval_order) {
            static const std::vector<int32_t> empty_ids;
            const auto& ids = predicate_id < static_cast<int>(resolved.global_predicates.size())
                ? resolved.global_predicates[predicate_id]
                : empty_ids;
            eval_predicate(plan.predicates[predicate_id], cols, disp, ids, r0, m,
                           options.enable_prefetch, scratch.zones,
                           scratch.predicate_masks[predicate_id]);
        }

        std::vector<int64_t>& counts = count_pool.local();
        uint8_t* uni_morsel = uni.data() + (r0 >> 3);
        for (int r = 0; r < R; ++r) {
            scratch.arena.reset();
            const RuleEvalPlan& rule_plan = plan.rule_plans[r];
            scratch.node_masks.assign(rule_plan.nodes.size(), nullptr);
            ExprEvaluator evaluator{
                rule_plan, scratch.predicate_masks, disp, options, scratch.arena,
                scratch.node_masks, scratch.survivors, scratch.next_survivors, m};
            const uint8_t* final_mask = evaluator.eval(rule_plan.root_node);

            if (materialize) std::memcpy(out_ptr[r] + (r0 >> 3), final_mask, mb);
            counts[r] += blazerules::count_set_bits(final_mask, mb);
            blazerules::or_into(uni_morsel, final_mask, mb);

            if (!reduce_decisions) continue;
            const RuleReduce& rr = reduce[static_cast<size_t>(r)];
            if (rr.shadow) continue;
            for (int b = 0; b < mb; ++b) {
                uint8_t byte = final_mask[b];
                while (byte != 0) {
                    int bit = blazerules::ctz32(byte);
                    int j = b * 8 + bit;
                    if (j >= m) break;
                    int row = r0 + j;
                    if (need_scores) {
                        double s = out.scores[static_cast<size_t>(row)] + rr.contribution;
                        if (rr.score_cap > 0.0 && s > rr.score_cap) s = rr.score_cap;
                        out.scores[static_cast<size_t>(row)] = s;
                    }
                    bool wins = rr.action_rank > best_action[row] ||
                        (rr.action_rank == best_action[row] && rr.severity_rank > best_severity[row]) ||
                        (rr.action_rank == best_action[row] && rr.severity_rank == best_severity[row] &&
                         rr.priority > best_priority[row]);
                    if (wins) {
                        best_action[row] = rr.action_rank;
                        if (need_codes) out.decision_codes[static_cast<size_t>(row)] = rr.action_code;
                        best_severity[row] = rr.severity_rank;
                        best_priority[row] = rr.priority;
                        if (need_winning) winning_idx[static_cast<size_t>(row)] = r;
                    }
                    byte &= byte - 1;
                }
            }
        }
    };

    int num_morsels = (n + MORSEL_ROWS - 1) / MORSEL_ROWS;
    if (n <= options.parallel_threshold || num_morsels <= 1) {
        for (int mi = 0; mi < num_morsels; ++mi) do_morsel(mi);
    } else {
        tbb::parallel_for(tbb::blocked_range<int>(0, num_morsels),
                          [&](const tbb::blocked_range<int>& range) {
                              for (int mi = range.begin(); mi != range.end(); ++mi) do_morsel(mi);
                          });
    }

    auto assemble_start = std::chrono::steady_clock::now();
    std::vector<int64_t> total_counts(static_cast<size_t>(R), 0);
    for (const auto& partial : count_pool) {
        for (int r = 0; r < R; ++r) total_counts[r] += partial[r];
    }
    for (int r = 0; r < R; ++r) {
        out.rule_match_counts[rs.rules[r].rule_id] = static_cast<int>(total_counts[r]);
    }
    out.n_matched = blazerules::count_set_bits(uni.data(), bb);
    if (options.materialize_matched_indices) {
        blazerules::find_set_bits(uni.data(), n, out.matched_record_indices);
    }
    if (reduce_decisions) {
        for (int i = 0; i < n; ++i) {
            int code = need_codes ? out.decision_codes[static_cast<size_t>(i)] : default_code;
            std::string label = default_label;
            if (code >= 0 && code < static_cast<int>(rs.decision_labels.size())) {
                label = rs.decision_labels[static_cast<size_t>(code)];
            }
            if (options.materialize_decision_strings) {
                out.decisions[static_cast<size_t>(i)] = label;
            }
            std::string winning_rule;
            if (need_winning && winning_idx[static_cast<size_t>(i)] >= 0) {
                winning_rule = rs.rules[static_cast<size_t>(winning_idx[static_cast<size_t>(i)])].rule_id;
                if (options.materialize_winning_rules) {
                    out.winning_rule_ids[static_cast<size_t>(i)] = winning_rule;
                }
            }
            if (options.materialize_grouped_indices) {
                out.grouped_decision_indices[label].push_back(i);
                if (!winning_rule.empty()) {
                    out.grouped_winning_rule_indices[winning_rule].push_back(i);
                }
            }
            if (options.materialize_risk_bands) {
                const double score = need_scores ? out.scores[static_cast<size_t>(i)] : 0.0;
                out.risk_bands[static_cast<size_t>(i)] = risk_band_for(score, label);
            }
        }
    }
    out.timing.result_assemble_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - assemble_start).count();
}
