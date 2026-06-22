#include "blazerules/simd_kernels.h"

#include <cstdint>
#include <cstring>

#include <immintrin.h>

const KernelDispatchTable& avx2_kernel_table();

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

template <int OP>
inline __mmask16 cmp_f32(__m512 a, __m512 b) {
    if constexpr (OP == OP_GT)  return _mm512_cmp_ps_mask(a, b, _CMP_GT_OQ);
    if constexpr (OP == OP_LT)  return _mm512_cmp_ps_mask(a, b, _CMP_LT_OQ);
    if constexpr (OP == OP_GTE) return _mm512_cmp_ps_mask(a, b, _CMP_GE_OQ);
    if constexpr (OP == OP_LTE) return _mm512_cmp_ps_mask(a, b, _CMP_LE_OQ);
    if constexpr (OP == OP_EQ)  return _mm512_cmp_ps_mask(a, b, _CMP_EQ_OQ);
    if constexpr (OP == OP_NEQ) return _mm512_cmp_ps_mask(a, b, _CMP_NEQ_OQ);
}

template <int OP>
inline __mmask8 cmp_f64(__m512d a, __m512d b) {
    if constexpr (OP == OP_GT)  return _mm512_cmp_pd_mask(a, b, _CMP_GT_OQ);
    if constexpr (OP == OP_LT)  return _mm512_cmp_pd_mask(a, b, _CMP_LT_OQ);
    if constexpr (OP == OP_GTE) return _mm512_cmp_pd_mask(a, b, _CMP_GE_OQ);
    if constexpr (OP == OP_LTE) return _mm512_cmp_pd_mask(a, b, _CMP_LE_OQ);
    if constexpr (OP == OP_EQ)  return _mm512_cmp_pd_mask(a, b, _CMP_EQ_OQ);
    if constexpr (OP == OP_NEQ) return _mm512_cmp_pd_mask(a, b, _CMP_NEQ_OQ);
}

template <int OP>
inline __mmask16 cmp_i32(__m512i a, __m512i b) {
    if constexpr (OP == OP_GT)  return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_GT);
    if constexpr (OP == OP_LT)  return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_LT);
    if constexpr (OP == OP_GTE) return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_GE);
    if constexpr (OP == OP_LTE) return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_LE);
    if constexpr (OP == OP_EQ)  return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_EQ);
    if constexpr (OP == OP_NEQ) return _mm512_cmp_epi32_mask(a, b, _MM_CMPINT_NE);
}

template <int OP>
inline __mmask8 cmp_i64(__m512i a, __m512i b) {
    if constexpr (OP == OP_GT)  return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_GT);
    if constexpr (OP == OP_LT)  return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_LT);
    if constexpr (OP == OP_GTE) return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_GE);
    if constexpr (OP == OP_LTE) return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_LE);
    if constexpr (OP == OP_EQ)  return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_EQ);
    if constexpr (OP == OP_NEQ) return _mm512_cmp_epi64_mask(a, b, _MM_CMPINT_NE);
}

template <class T, int OP>
void scalar_tail(const T* d, const uint8_t* vp, T threshold, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (scmp<T, OP>(d[b * 8 + i], threshold)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (scmp<T, OP>(d[nb * 8 + i], threshold)) byte |= static_cast<uint8_t>(1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <int OP>
void eval_f32_avx512(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const float* d = static_cast<const float*>(dp);
    const __m512 t = _mm512_set1_ps(static_cast<float>(thr));
    int i = 0, ob = 0;
    for (; i + 16 <= n; i += 16, ob += 2) {
        uint16_t mask = static_cast<uint16_t>(cmp_f32<OP>(_mm512_loadu_ps(d + i), t));
        out[ob] = static_cast<uint8_t>(mask & 0xffu);
        out[ob + 1] = static_cast<uint8_t>((mask >> 8) & 0xffu);
        if (vp) {
            out[ob] &= vp[ob];
            out[ob + 1] &= vp[ob + 1];
        }
    }
    if (i < n) scalar_tail<float, OP>(d + i, vp ? vp + ob : nullptr, static_cast<float>(thr), out + ob, n - i);
}

template <int OP>
void eval_i32_avx512(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int32_t* d = static_cast<const int32_t*>(dp);
    const __m512i t = _mm512_set1_epi32(static_cast<int32_t>(thr));
    int i = 0, ob = 0;
    for (; i + 16 <= n; i += 16, ob += 2) {
        uint16_t mask = static_cast<uint16_t>(cmp_i32<OP>(_mm512_loadu_si512(reinterpret_cast<const void*>(d + i)), t));
        out[ob] = static_cast<uint8_t>(mask & 0xffu);
        out[ob + 1] = static_cast<uint8_t>((mask >> 8) & 0xffu);
        if (vp) {
            out[ob] &= vp[ob];
            out[ob + 1] &= vp[ob + 1];
        }
    }
    if (i < n) scalar_tail<int32_t, OP>(d + i, vp ? vp + ob : nullptr, static_cast<int32_t>(thr), out + ob, n - i);
}

template <int OP>
void eval_f64_avx512(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const double* d = static_cast<const double*>(dp);
    const __m512d t = _mm512_set1_pd(thr);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = static_cast<uint8_t>(cmp_f64<OP>(_mm512_loadu_pd(d + b * 8), t));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) scalar_tail<double, OP>(d + nb * 8, vp ? vp + nb : nullptr, thr, out + nb, rem);
}

template <int OP>
void eval_i64_avx512(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int64_t* d = static_cast<const int64_t*>(dp);
    const __m512i t = _mm512_set1_epi64(static_cast<int64_t>(thr));
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = static_cast<uint8_t>(cmp_i64<OP>(_mm512_loadu_si512(reinterpret_cast<const void*>(d + b * 8)), t));
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) scalar_tail<int64_t, OP>(d + nb * 8, vp ? vp + nb : nullptr, static_cast<int64_t>(thr), out + nb, rem);
}

void bitwise_and_avx512(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
        for (; i + 64 <= nbytes; i += 64) {
            __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(out + i));
            __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(in + i));
            _mm512_storeu_si512(reinterpret_cast<void*>(out + i), _mm512_and_si512(a, b));
        }
        for (; i < nbytes; ++i) out[i] &= in[i];
    }
}

void bitwise_or_avx512(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
        for (; i + 64 <= nbytes; i += 64) {
            __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(out + i));
            __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(in + i));
            _mm512_storeu_si512(reinterpret_cast<void*>(out + i), _mm512_or_si512(a, b));
        }
        for (; i < nbytes; ++i) out[i] |= in[i];
    }
}

void bitwise_not_avx512(const uint8_t* input, uint8_t* out, int nbytes, int n_records) {
    const __m512i all = _mm512_set1_epi8(static_cast<char>(0xff));
    int i = 0;
    for (; i + 64 <= nbytes; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(input + i));
        _mm512_storeu_si512(reinterpret_cast<void*>(out + i), _mm512_xor_si512(v, all));
    }
    for (; i < nbytes; ++i) out[i] = static_cast<uint8_t>(~input[i]);
    int tail = n_records & 7;
    if (tail != 0 && nbytes > 0) out[nbytes - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
}

float dot_f32_avx512(const float* a, const float* b, int d) {
    __m512 acc0 = _mm512_setzero_ps();
    __m512 acc1 = _mm512_setzero_ps();
    int i = 0;
    for (; i + 32 <= d; i += 32) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc0);
        acc1 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16), acc1);
    }
    for (; i + 16 <= d; i += 16) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc0);
    }
    float sum = _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc1));
    for (; i < d; ++i) sum += a[i] * b[i];
    return sum;
}

float l2sq_f32_avx512(const float* a, const float* b, int d) {
    __m512 acc0 = _mm512_setzero_ps();
    __m512 acc1 = _mm512_setzero_ps();
    int i = 0;
    for (; i + 32 <= d; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        acc0 = _mm512_fmadd_ps(d0, d0, acc0);
        acc1 = _mm512_fmadd_ps(d1, d1, acc1);
    }
    for (; i + 16 <= d; i += 16) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        acc0 = _mm512_fmadd_ps(d0, d0, acc0);
    }
    float sum = _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc1));
    for (; i < d; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float norm_sq_f32_avx512(const float* a, int d) {
    return dot_f32_avx512(a, a, d);
}

KernelDispatchTable build_avx512_table() {
    KernelDispatchTable t = avx2_kernel_table();
#define SET_ROW(idx, fn) \
    t.numeric[idx][OP_GT]  = &fn<OP_GT>;  t.numeric[idx][OP_LT]  = &fn<OP_LT>; \
    t.numeric[idx][OP_GTE] = &fn<OP_GTE>; t.numeric[idx][OP_LTE] = &fn<OP_LTE>; \
    t.numeric[idx][OP_EQ]  = &fn<OP_EQ>;  t.numeric[idx][OP_NEQ] = &fn<OP_NEQ>;
    SET_ROW(0, eval_f32_avx512)
    SET_ROW(1, eval_f64_avx512)
    SET_ROW(2, eval_i32_avx512)
    SET_ROW(3, eval_i64_avx512)
#undef SET_ROW
    t.bitwise_and = &bitwise_and_avx512;
    t.bitwise_or = &bitwise_or_avx512;
    t.bitwise_not = &bitwise_not_avx512;
    t.dot_f32 = &dot_f32_avx512;
    t.l2sq_f32 = &l2sq_f32_avx512;
    t.norm_sq_f32 = &norm_sq_f32_avx512;
    return t;
}

} // namespace

const KernelDispatchTable& avx512_kernel_table() {
    static const KernelDispatchTable table = build_avx512_table();
    return table;
}
