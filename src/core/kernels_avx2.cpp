#include "blazerules/simd_kernels.h"

#include <cstdint>
#include <cstring>

#include <immintrin.h>

namespace {

enum { OP_GT = 0, OP_LT = 1, OP_GTE = 2, OP_LTE = 3, OP_EQ = 4, OP_NEQ = 5 };

template <class T, int OP>
inline bool scmp(T a, T b) {
    if constexpr (OP == OP_GT)  return a > b;
    if constexpr (OP == OP_LT)  return a < b;
    if constexpr (OP == OP_GTE) return a >= b;
    if constexpr (OP == OP_LTE) return a <= b;
    if constexpr (OP == OP_EQ)  return a == b;
    if constexpr (OP == OP_NEQ) return a != b;
}

inline __m256 cmp_f32(__m256 a, __m256 b, int op) {
    switch (op) {
        case OP_GT:  return _mm256_cmp_ps(a, b, _CMP_GT_OQ);
        case OP_LT:  return _mm256_cmp_ps(a, b, _CMP_LT_OQ);
        case OP_GTE: return _mm256_cmp_ps(a, b, _CMP_GE_OQ);
        case OP_LTE: return _mm256_cmp_ps(a, b, _CMP_LE_OQ);
        case OP_EQ:  return _mm256_cmp_ps(a, b, _CMP_EQ_OQ);
        case OP_NEQ: return _mm256_cmp_ps(a, b, _CMP_NEQ_OQ);
        default:     return _mm256_setzero_ps();
    }
}

inline __m256d cmp_f64(__m256d a, __m256d b, int op) {
    switch (op) {
        case OP_GT:  return _mm256_cmp_pd(a, b, _CMP_GT_OQ);
        case OP_LT:  return _mm256_cmp_pd(a, b, _CMP_LT_OQ);
        case OP_GTE: return _mm256_cmp_pd(a, b, _CMP_GE_OQ);
        case OP_LTE: return _mm256_cmp_pd(a, b, _CMP_LE_OQ);
        case OP_EQ:  return _mm256_cmp_pd(a, b, _CMP_EQ_OQ);
        case OP_NEQ: return _mm256_cmp_pd(a, b, _CMP_NEQ_OQ);
        default:     return _mm256_setzero_pd();
    }
}

template <int OP>
inline __m256i cmp_i32(__m256i a, __m256i b) {
    if constexpr (OP == OP_GT) {
        return _mm256_cmpgt_epi32(a, b);
    } else if constexpr (OP == OP_LT) {
        return _mm256_cmpgt_epi32(b, a);
    } else if constexpr (OP == OP_EQ) {
        return _mm256_cmpeq_epi32(a, b);
    } else {
        __m256i eq = _mm256_cmpeq_epi32(a, b);
        __m256i gt = _mm256_cmpgt_epi32(a, b);
        __m256i lt = _mm256_cmpgt_epi32(b, a);
        const __m256i all = _mm256_cmpeq_epi32(eq, eq);
        if constexpr (OP == OP_GTE) return _mm256_xor_si256(lt, all);
        if constexpr (OP == OP_LTE) return _mm256_xor_si256(gt, all);
        if constexpr (OP == OP_NEQ) return _mm256_xor_si256(eq, all);
    }
}

template <int OP>
inline __m256i cmp_i64(__m256i a, __m256i b) {
    if constexpr (OP == OP_GT) {
        return _mm256_cmpgt_epi64(a, b);
    } else if constexpr (OP == OP_LT) {
        return _mm256_cmpgt_epi64(b, a);
    } else if constexpr (OP == OP_EQ) {
        return _mm256_cmpeq_epi64(a, b);
    } else {
        __m256i eq = _mm256_cmpeq_epi64(a, b);
        __m256i gt = _mm256_cmpgt_epi64(a, b);
        __m256i lt = _mm256_cmpgt_epi64(b, a);
        const __m256i all = _mm256_cmpeq_epi64(eq, eq);
        if constexpr (OP == OP_GTE) return _mm256_xor_si256(lt, all);
        if constexpr (OP == OP_LTE) return _mm256_xor_si256(gt, all);
        if constexpr (OP == OP_NEQ) return _mm256_xor_si256(eq, all);
    }
}

template <int OP>
void eval_f32_avx2(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const float* d = static_cast<const float*>(dp);
    const __m256 t = _mm256_set1_ps(static_cast<float>(thr));
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = static_cast<uint8_t>(_mm256_movemask_ps(cmp_f32(_mm256_loadu_ps(d + b * 8), t, OP)));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        float t_scalar = static_cast<float>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<float, OP>(d[nb * 8 + i], t_scalar)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <int OP>
void eval_f64_avx2(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const double* d = static_cast<const double*>(dp);
    const __m256d t = _mm256_set1_pd(thr);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const double* p = d + b * 8;
        int lo = _mm256_movemask_pd(cmp_f64(_mm256_loadu_pd(p), t, OP));
        int hi = _mm256_movemask_pd(cmp_f64(_mm256_loadu_pd(p + 4), t, OP));
        uint8_t byte = static_cast<uint8_t>(lo | (hi << 4));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (scmp<double, OP>(d[nb * 8 + i], thr)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <int OP>
void eval_i32_avx2(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int32_t* d = static_cast<const int32_t*>(dp);
    const __m256i t = _mm256_set1_epi32(static_cast<int32_t>(thr));
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        __m256i cmp = cmp_i32<OP>(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(d + b * 8)), t);
        uint8_t byte = static_cast<uint8_t>(_mm256_movemask_ps(_mm256_castsi256_ps(cmp)));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        int32_t t_scalar = static_cast<int32_t>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<int32_t, OP>(d[nb * 8 + i], t_scalar)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <int OP>
void eval_i64_avx2(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int64_t* d = static_cast<const int64_t*>(dp);
    const __m256i t = _mm256_set1_epi64x(static_cast<int64_t>(thr));
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const int64_t* p = d + b * 8;
        __m256i cmp0 = cmp_i64<OP>(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), t);
        __m256i cmp1 = cmp_i64<OP>(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 4)), t);
        int lo = _mm256_movemask_pd(_mm256_castsi256_pd(cmp0));
        int hi = _mm256_movemask_pd(_mm256_castsi256_pd(cmp1));
        uint8_t byte = static_cast<uint8_t>(lo | (hi << 4));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        int64_t t_scalar = static_cast<int64_t>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<int64_t, OP>(d[nb * 8 + i], t_scalar)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <class T, bool INCL>
void eval_range_avx2_scalar_tail(const void* dp, const uint8_t* vp, double lo, double hi, uint8_t* out, int n) {
    const T* d = static_cast<const T*>(dp);
    const T l = static_cast<T>(lo), h = static_cast<T>(hi);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const T* base = d + b * 8;
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) {
            T v = base[i];
            bool c = INCL ? (v >= l && v <= h) : (v > l && v < h);
            if (c) byte |= static_cast<uint8_t>(1u << i);
        }
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) {
            T v = d[nb * 8 + i];
            bool c = INCL ? (v >= l && v <= h) : (v > l && v < h);
            if (c) byte |= static_cast<uint8_t>(1u << i);
        }
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <bool INCL>
void eval_range_f32_avx2(const void* dp, const uint8_t* vp, double lo, double hi, uint8_t* out, int n) {
    const float* d = static_cast<const float*>(dp);
    const __m256 vlo = _mm256_set1_ps(static_cast<float>(lo));
    const __m256 vhi = _mm256_set1_ps(static_cast<float>(hi));
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        __m256 v = _mm256_loadu_ps(d + b * 8);
        __m256 ge = _mm256_cmp_ps(v, vlo, INCL ? _CMP_GE_OQ : _CMP_GT_OQ);
        __m256 le = _mm256_cmp_ps(v, vhi, INCL ? _CMP_LE_OQ : _CMP_LT_OQ);
        uint8_t byte = static_cast<uint8_t>(_mm256_movemask_ps(_mm256_and_ps(ge, le)));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) eval_range_avx2_scalar_tail<float, INCL>(d + nb * 8, vp ? vp + nb : nullptr, lo, hi, out + nb, rem);
}

template <bool INCL>
void eval_range_i32_avx2(const void* dp, const uint8_t* vp, double lo, double hi, uint8_t* out, int n) {
    const int32_t* d = static_cast<const int32_t*>(dp);
    const __m256i vlo = _mm256_set1_epi32(static_cast<int32_t>(lo));
    const __m256i vhi = _mm256_set1_epi32(static_cast<int32_t>(hi));
    const __m256i all = _mm256_cmpeq_epi32(vlo, vlo);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(d + b * 8));
        __m256i low_fail = INCL ? _mm256_cmpgt_epi32(vlo, v) : _mm256_xor_si256(_mm256_cmpgt_epi32(v, vlo), all);
        __m256i high_fail = INCL ? _mm256_cmpgt_epi32(v, vhi) : _mm256_xor_si256(_mm256_cmpgt_epi32(vhi, v), all);
        __m256i pass = _mm256_andnot_si256(_mm256_or_si256(low_fail, high_fail), all);
        uint8_t byte = static_cast<uint8_t>(_mm256_movemask_ps(_mm256_castsi256_ps(pass)));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) eval_range_avx2_scalar_tail<int32_t, INCL>(d + nb * 8, vp ? vp + nb : nullptr, lo, hi, out + nb, rem);
}

inline bool in_set(int32_t v, const int32_t* ids, int nids) {
    for (int i = 0; i < nids; ++i) if (v == ids[i]) return true;
    return false;
}

inline uint8_t eval_in_dict8_avx2(const int32_t* d, const int32_t* ids, int nids) {
    __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(d));
    __m256i mask = _mm256_setzero_si256();
    for (int k = 0; k < nids; ++k) {
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi32(values, _mm256_set1_epi32(ids[k])));
    }
    return static_cast<uint8_t>(_mm256_movemask_ps(_mm256_castsi256_ps(mask)));
}

void eval_in_dict_avx2(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = nids > 0 && nids <= 4 ? eval_in_dict8_avx2(d + b * 8, ids, nids) : 0;
        if (nids <= 0 || nids > 4) {
            for (int i = 0; i < 8; ++i) if (in_set(d[b * 8 + i], ids, nids)) byte |= static_cast<uint8_t>(1u << i);
        }
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (in_set(d[nb * 8 + i], ids, nids)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void eval_not_in_dict_avx2(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = nids > 0 && nids <= 4 ? static_cast<uint8_t>(~eval_in_dict8_avx2(d + b * 8, ids, nids)) : 0xff;
        if (nids <= 0 || nids > 4) {
            byte = 0;
            for (int i = 0; i < 8; ++i) if (!in_set(d[b * 8 + i], ids, nids)) byte |= static_cast<uint8_t>(1u << i);
        }
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (!in_set(d[nb * 8 + i], ids, nids)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void eval_eq_dict_avx2(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int32_t id = nids > 0 ? ids[0] : -1;
    eval_in_dict_avx2(d, vp, &id, 1, out, n);
}

void bitwise_and_avx2(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
        for (; i + 32 <= nbytes; i += 32) {
            __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + i));
            __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_and_si256(a, b));
        }
        for (; i < nbytes; ++i) out[i] &= in[i];
    }
}

void bitwise_or_avx2(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
        for (; i + 32 <= nbytes; i += 32) {
            __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + i));
            __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_or_si256(a, b));
        }
        for (; i < nbytes; ++i) out[i] |= in[i];
    }
}

void bitwise_not_avx2(const uint8_t* input, uint8_t* out, int nbytes, int n_records) {
    const __m256i all = _mm256_set1_epi8(static_cast<char>(0xff));
    int i = 0;
    for (; i + 32 <= nbytes; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), _mm256_xor_si256(v, all));
    }
    for (; i < nbytes; ++i) out[i] = static_cast<uint8_t>(~input[i]);
    int tail = n_records & 7;
    if (tail != 0 && nbytes > 0) out[nbytes - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
}

float hsum_ps(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return _mm_cvtss_f32(sum);
}

float dot_f32_avx2(const float* a, const float* b, int d) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= d; i += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
    }
    for (; i + 8 <= d; i += 8) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
    }
    float sum = hsum_ps(_mm256_add_ps(acc0, acc1));
    for (; i < d; ++i) sum += a[i] * b[i];
    return sum;
}

float l2sq_f32_avx2(const float* a, const float* b, int d) {
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= d; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
        acc1 = _mm256_fmadd_ps(d1, d1, acc1);
    }
    for (; i + 8 <= d; i += 8) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    }
    float sum = hsum_ps(_mm256_add_ps(acc0, acc1));
    for (; i < d; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float norm_sq_f32_avx2(const float* a, int d) {
    return dot_f32_avx2(a, a, d);
}

KernelDispatchTable build_avx2_table() {
    KernelDispatchTable t{};
#define SET_ROW(idx, fn) \
    t.numeric[idx][OP_GT]  = &fn<OP_GT>;  t.numeric[idx][OP_LT]  = &fn<OP_LT>; \
    t.numeric[idx][OP_GTE] = &fn<OP_GTE>; t.numeric[idx][OP_LTE] = &fn<OP_LTE>; \
    t.numeric[idx][OP_EQ]  = &fn<OP_EQ>;  t.numeric[idx][OP_NEQ] = &fn<OP_NEQ>;
    SET_ROW(0, eval_f32_avx2)
    SET_ROW(1, eval_f64_avx2)
    SET_ROW(2, eval_i32_avx2)
    SET_ROW(3, eval_i64_avx2)
#undef SET_ROW
    t.range[0][0] = &eval_range_f32_avx2<true>;
    t.range[0][1] = &eval_range_f32_avx2<false>;
    t.range[1][0] = &eval_range_avx2_scalar_tail<double, true>;
    t.range[1][1] = &eval_range_avx2_scalar_tail<double, false>;
    t.range[2][0] = &eval_range_i32_avx2<true>;
    t.range[2][1] = &eval_range_i32_avx2<false>;
    t.range[3][0] = &eval_range_avx2_scalar_tail<int64_t, true>;
    t.range[3][1] = &eval_range_avx2_scalar_tail<int64_t, false>;
    t.string_[0] = &eval_in_dict_avx2;
    t.string_[1] = &eval_not_in_dict_avx2;
    t.string_[2] = &eval_eq_dict_avx2;
    t.bitwise_and = &bitwise_and_avx2;
    t.bitwise_or = &bitwise_or_avx2;
    t.bitwise_not = &bitwise_not_avx2;
    t.dot_f32 = &dot_f32_avx2;
    t.l2sq_f32 = &l2sq_f32_avx2;
    t.norm_sq_f32 = &norm_sq_f32_avx2;
    return t;
}

} // namespace

const KernelDispatchTable& avx2_kernel_table() {
    static const KernelDispatchTable table = build_avx2_table();
    return table;
}
