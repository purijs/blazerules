//
// Numeric / range / categorical SIMD kernels + dispatch table population.
//

#include "blazerules/simd_kernels.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#if defined(__aarch64__)
#include <arm_neon.h>
#define BLAZERULES_NEON 1
#else
#define BLAZERULES_NEON 0
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#endif

#if defined(BLAZERULES_BUILD_AVX2)
const KernelDispatchTable& avx2_kernel_table();
#endif

#if defined(BLAZERULES_BUILD_AVX512)
const KernelDispatchTable& avx512_kernel_table();
#endif

namespace {

enum { OP_GT = 0, OP_LT = 1, OP_GTE = 2, OP_LTE = 3, OP_EQ = 4, OP_NEQ = 5 };

constexpr int BACKEND_AUTO = -1;
std::atomic<int> g_forced_backend{BACKEND_AUTO};
std::atomic<bool> g_enable_avx512_auto{false};

template <class T, int OP>
inline bool scmp(T a, T b) {
    if constexpr (OP == OP_GT)  return a > b;
    if constexpr (OP == OP_LT)  return a < b;
    if constexpr (OP == OP_GTE) return a >= b;
    if constexpr (OP == OP_LTE) return a <= b;
    if constexpr (OP == OP_EQ)  return a == b;
    if constexpr (OP == OP_NEQ) return a != b;
}

// ── Generic scalar numeric kernel (universal fallback, auto-vectorizes) ──────
template <class T, int OP>
void eval_num_scalar(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const T* d = static_cast<const T*>(dp);
    const T t = static_cast<T>(thr);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const T* base = d + b * 8;
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i)
            if (scmp<T, OP>(base[i], t)) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i)
            if (scmp<T, OP>(d[nb * 8 + i], t)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

template <int OP> void eval_f32_s(const void* dp, const uint8_t* vp, double t, uint8_t* o, int n) { eval_num_scalar<float, OP>(dp, vp, t, o, n); }
template <int OP> void eval_f64_s(const void* dp, const uint8_t* vp, double t, uint8_t* o, int n) { eval_num_scalar<double, OP>(dp, vp, t, o, n); }
template <int OP> void eval_i32_s(const void* dp, const uint8_t* vp, double t, uint8_t* o, int n) { eval_num_scalar<int32_t, OP>(dp, vp, t, o, n); }
template <int OP> void eval_i64_s(const void* dp, const uint8_t* vp, double t, uint8_t* o, int n) { eval_num_scalar<int64_t, OP>(dp, vp, t, o, n); }

#if BLAZERULES_NEON
// Bit-weight vectors used to pack 4 comparison-mask lanes into a nibble via a
// horizontal add (NEON has no movemask). 32-bit lanes for f32/i32.
inline uint32x4_t wlo32() { static const uint32_t W[4] = {1, 2, 4, 8};   return vld1q_u32(W); }
inline uint32x4_t whi32() { static const uint32_t W[4] = {16, 32, 64, 128}; return vld1q_u32(W); }
// 64-bit lanes for f64/i64 (2 lanes per vector, 4 vectors per byte).
inline uint64x2_t w64(uint64_t a, uint64_t b) { uint64_t W[2] = {a, b}; return vld1q_u64(W); }

template <int OP> inline uint32x4_t cmp_f32(float32x4_t a, float32x4_t b) {
    if constexpr (OP == OP_GT)  return vcgtq_f32(a, b);
    if constexpr (OP == OP_LT)  return vcltq_f32(a, b);
    if constexpr (OP == OP_GTE) return vcgeq_f32(a, b);
    if constexpr (OP == OP_LTE) return vcleq_f32(a, b);
    if constexpr (OP == OP_EQ)  return vceqq_f32(a, b);
    if constexpr (OP == OP_NEQ) return vmvnq_u32(vceqq_f32(a, b));
}
template <int OP> inline uint32x4_t cmp_i32(int32x4_t a, int32x4_t b) {
    if constexpr (OP == OP_GT)  return vcgtq_s32(a, b);
    if constexpr (OP == OP_LT)  return vcltq_s32(a, b);
    if constexpr (OP == OP_GTE) return vcgeq_s32(a, b);
    if constexpr (OP == OP_LTE) return vcleq_s32(a, b);
    if constexpr (OP == OP_EQ)  return vceqq_s32(a, b);
    if constexpr (OP == OP_NEQ) return vmvnq_u32(vceqq_s32(a, b));
}
template <int OP> inline uint64x2_t cmp_f64(float64x2_t a, float64x2_t b) {
    if constexpr (OP == OP_GT)  return vcgtq_f64(a, b);
    if constexpr (OP == OP_LT)  return vcltq_f64(a, b);
    if constexpr (OP == OP_GTE) return vcgeq_f64(a, b);
    if constexpr (OP == OP_LTE) return vcleq_f64(a, b);
    if constexpr (OP == OP_EQ)  return vceqq_f64(a, b);
    if constexpr (OP == OP_NEQ) return veorq_u64(vceqq_f64(a, b), vdupq_n_u64(~0ull));
}
template <int OP> inline uint64x2_t cmp_i64(int64x2_t a, int64x2_t b) {
    if constexpr (OP == OP_GT)  return vcgtq_s64(a, b);
    if constexpr (OP == OP_LT)  return vcltq_s64(a, b);
    if constexpr (OP == OP_GTE) return vcgeq_s64(a, b);
    if constexpr (OP == OP_LTE) return vcleq_s64(a, b);
    if constexpr (OP == OP_EQ)  return vceqq_s64(a, b);
    if constexpr (OP == OP_NEQ) return veorq_u64(vceqq_s64(a, b), vdupq_n_u64(~0ull));
}

template <int OP> void eval_f32_n(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const float* d = static_cast<const float*>(dp);
    const float32x4_t vt = vdupq_n_f32(static_cast<float>(thr));
    const uint32x4_t lo = wlo32(), hi = whi32();
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint32x4_t c0 = cmp_f32<OP>(vld1q_f32(d + b * 8), vt);
        uint32x4_t c1 = cmp_f32<OP>(vld1q_f32(d + b * 8 + 4), vt);
        uint32_t byte = vaddvq_u32(vandq_u32(c0, lo)) | vaddvq_u32(vandq_u32(c1, hi));
        if (vp) byte &= vp[b];
        out[b] = static_cast<uint8_t>(byte);
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0; const float t = static_cast<float>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<float, OP>(d[nb * 8 + i], t)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
template <int OP> void eval_i32_n(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int32_t* d = static_cast<const int32_t*>(dp);
    const int32x4_t vt = vdupq_n_s32(static_cast<int32_t>(thr));
    const uint32x4_t lo = wlo32(), hi = whi32();
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint32x4_t c0 = cmp_i32<OP>(vld1q_s32(d + b * 8), vt);
        uint32x4_t c1 = cmp_i32<OP>(vld1q_s32(d + b * 8 + 4), vt);
        uint32_t byte = vaddvq_u32(vandq_u32(c0, lo)) | vaddvq_u32(vandq_u32(c1, hi));
        if (vp) byte &= vp[b];
        out[b] = static_cast<uint8_t>(byte);
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0; const int32_t t = static_cast<int32_t>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<int32_t, OP>(d[nb * 8 + i], t)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
template <int OP> void eval_f64_n(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const double* d = static_cast<const double*>(dp);
    const float64x2_t vt = vdupq_n_f64(thr);
    const uint64x2_t w0 = w64(1, 2), w1 = w64(4, 8), w2 = w64(16, 32), w3 = w64(64, 128);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const double* p = d + b * 8;
        uint64_t byte = vaddvq_u64(vandq_u64(cmp_f64<OP>(vld1q_f64(p),     vt), w0))
                      | vaddvq_u64(vandq_u64(cmp_f64<OP>(vld1q_f64(p + 2), vt), w1))
                      | vaddvq_u64(vandq_u64(cmp_f64<OP>(vld1q_f64(p + 4), vt), w2))
                      | vaddvq_u64(vandq_u64(cmp_f64<OP>(vld1q_f64(p + 6), vt), w3));
        if (vp) byte &= vp[b];
        out[b] = static_cast<uint8_t>(byte);
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (scmp<double, OP>(d[nb * 8 + i], thr)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
template <int OP> void eval_i64_n(const void* dp, const uint8_t* vp, double thr, uint8_t* out, int n) {
    const int64_t* d = static_cast<const int64_t*>(dp);
    const int64x2_t vt = vdupq_n_s64(static_cast<int64_t>(thr));
    const uint64x2_t w0 = w64(1, 2), w1 = w64(4, 8), w2 = w64(16, 32), w3 = w64(64, 128);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const int64_t* p = d + b * 8;
        uint64_t byte = vaddvq_u64(vandq_u64(cmp_i64<OP>(vld1q_s64(p),     vt), w0))
                      | vaddvq_u64(vandq_u64(cmp_i64<OP>(vld1q_s64(p + 2), vt), w1))
                      | vaddvq_u64(vandq_u64(cmp_i64<OP>(vld1q_s64(p + 4), vt), w2))
                      | vaddvq_u64(vandq_u64(cmp_i64<OP>(vld1q_s64(p + 6), vt), w3));
        if (vp) byte &= vp[b];
        out[b] = static_cast<uint8_t>(byte);
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0; const int64_t t = static_cast<int64_t>(thr);
        for (int i = 0; i < rem; ++i) if (scmp<int64_t, OP>(d[nb * 8 + i], t)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
#endif // BLAZERULES_NEON

// ── Range kernel (between including / excluding) ─────────────────────────────
template <class T, bool INCL>
void eval_range(const void* dp, const uint8_t* vp, double lo, double hi, uint8_t* out, int n) {
    const T* d = static_cast<const T*>(dp);
    const T l = static_cast<T>(lo), h = static_cast<T>(hi);
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        const T* base = d + b * 8;
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) {
            T v = base[i];
            bool c = INCL ? (v >= l && v <= h) : (v > l && v < h);
            if (c) byte |= (1u << i);
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
            if (c) byte |= (1u << i);
        }
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

// ── Categorical (dictionary) kernels ─────────────────────────────────────────
inline bool in_set(int32_t v, const int32_t* ids, int nids) {
    for (int k = 0; k < nids; ++k) if (v == ids[k]) return true;
    return false;
}

#if BLAZERULES_NEON
inline uint8_t pack_i32_mask(uint32x4_t c0, uint32x4_t c1) {
    const uint32x4_t lo = wlo32(), hi = whi32();
    uint32_t byte = vaddvq_u32(vandq_u32(c0, lo)) | vaddvq_u32(vandq_u32(c1, hi));
    return static_cast<uint8_t>(byte);
}

inline uint8_t eval_in_dict_8_neon(const int32_t* d, const int32_t* ids, int nids) {
    int32x4_t v0 = vld1q_s32(d);
    int32x4_t v1 = vld1q_s32(d + 4);
    uint32x4_t m0 = vdupq_n_u32(0);
    uint32x4_t m1 = vdupq_n_u32(0);
    for (int k = 0; k < nids; ++k) {
        int32x4_t id = vdupq_n_s32(ids[k]);
        m0 = vorrq_u32(m0, vceqq_s32(v0, id));
        m1 = vorrq_u32(m1, vceqq_s32(v1, id));
    }
    return pack_i32_mask(m0, m1);
}
#endif

void eval_in_dict(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
#if BLAZERULES_NEON
        if (nids > 0 && nids <= 4) {
            uint8_t byte = eval_in_dict_8_neon(d + b * 8, ids, nids);
            if (vp) byte &= vp[b];
            out[b] = byte;
            continue;
        }
#endif
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (in_set(d[b * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (in_set(d[nb * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
void eval_not_in_dict(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
#if BLAZERULES_NEON
        if (nids > 0 && nids <= 4) {
            uint8_t byte = static_cast<uint8_t>(~eval_in_dict_8_neon(d + b * 8, ids, nids));
            if (vp) byte &= vp[b];
            out[b] = byte;
            continue;
        }
#endif
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (!in_set(d[b * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[b];   // null NOT IN -> still false (SQL semantics)
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (!in_set(d[nb * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}
void eval_eq_dict(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int32_t id = nids > 0 ? ids[0] : -1;
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
#if BLAZERULES_NEON
        if (nids > 0) {
            uint8_t byte = eval_in_dict_8_neon(d + b * 8, &id, 1);
            if (vp) byte &= vp[b];
            out[b] = byte;
            continue;
        }
#endif
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (d[b * 8 + i] == id) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (d[nb * 8 + i] == id) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void eval_in_dict_scalar(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (in_set(d[b * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (in_set(d[nb * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void eval_not_in_dict_scalar(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (!in_set(d[b * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (!in_set(d[nb * 8 + i], ids, nids)) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void eval_eq_dict_scalar(const int32_t* d, const uint8_t* vp, const int32_t* ids, int nids, uint8_t* out, int n) {
    int32_t id = nids > 0 ? ids[0] : -1;
    int nb = n / 8;
    for (int b = 0; b < nb; ++b) {
        uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) if (d[b * 8 + i] == id) byte |= (1u << i);
        if (vp) byte &= vp[b];
        out[b] = byte;
    }
    int rem = n - nb * 8;
    if (rem) {
        uint8_t byte = 0;
        for (int i = 0; i < rem; ++i) if (d[nb * 8 + i] == id) byte |= (1u << i);
        if (vp) byte &= vp[nb];
        out[nb] = byte;
    }
}

void bitwise_and_scalar(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        for (int i = 0; i < nbytes; ++i) out[i] &= in[i];
    }
}

void bitwise_or_scalar(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        for (int i = 0; i < nbytes; ++i) out[i] |= in[i];
    }
}

void bitwise_not_scalar(const uint8_t* input, uint8_t* out, int nbytes, int n_records) {
    for (int i = 0; i < nbytes; ++i) out[i] = static_cast<uint8_t>(~input[i]);
    int tail = n_records & 7;
    if (tail != 0 && nbytes > 0) {
        out[nbytes - 1] &= static_cast<uint8_t>((1u << tail) - 1u);
    }
}

void bitwise_and_kernel(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
#if BLAZERULES_NEON
        for (; i + 16 <= nbytes; i += 16) {
            uint8x16_t a = vld1q_u8(out + i);
            uint8x16_t b = vld1q_u8(in + i);
            vst1q_u8(out + i, vandq_u8(a, b));
        }
#endif
        for (; i < nbytes; ++i) out[i] &= in[i];
    }
}

void bitwise_or_kernel(const uint8_t* const* inputs, int n_inputs, uint8_t* out, int nbytes) {
    for (int k = 0; k < n_inputs; ++k) {
        const uint8_t* in = inputs[k];
        int i = 0;
#if BLAZERULES_NEON
        for (; i + 16 <= nbytes; i += 16) {
            uint8x16_t a = vld1q_u8(out + i);
            uint8x16_t b = vld1q_u8(in + i);
            vst1q_u8(out + i, vorrq_u8(a, b));
        }
#endif
        for (; i < nbytes; ++i) out[i] |= in[i];
    }
}

void bitwise_not_kernel(const uint8_t* input, uint8_t* out, int nbytes, int n_records) {
    int i = 0;
#if BLAZERULES_NEON
    const uint8x16_t all = vdupq_n_u8(0xff);
    for (; i + 16 <= nbytes; i += 16) {
        uint8x16_t a = vld1q_u8(input + i);
        vst1q_u8(out + i, veorq_u8(a, all));
    }
#endif
    for (; i < nbytes; ++i) out[i] = static_cast<uint8_t>(~input[i]);
    int tail = n_records & 7;
    if (tail != 0 && nbytes > 0) {
        uint8_t mask = static_cast<uint8_t>((1u << tail) - 1u);
        out[nbytes - 1] &= mask;
    }
}

float dot_f32_scalar(const float* a, const float* b, int d) {
    float sum = 0.0f;
    for (int i = 0; i < d; ++i) sum += a[i] * b[i];
    return sum;
}

float l2sq_f32_scalar(const float* a, const float* b, int d) {
    float sum = 0.0f;
    for (int i = 0; i < d; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float norm_sq_f32_scalar(const float* a, int d) {
    return dot_f32_scalar(a, a, d);
}

#if BLAZERULES_NEON
float dot_f32_neon(const float* a, const float* b, int d) {
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 8 <= d; i += 8) {
        acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
        acc1 = vfmaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    for (; i + 4 <= d; i += 4) {
        acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
    }
    float sum = vaddvq_f32(acc0) + vaddvq_f32(acc1);
    for (; i < d; ++i) sum += a[i] * b[i];
    return sum;
}

float l2sq_f32_neon(const float* a, const float* b, int d) {
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 8 <= d; i += 8) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        acc0 = vfmaq_f32(acc0, d0, d0);
        acc1 = vfmaq_f32(acc1, d1, d1);
    }
    for (; i + 4 <= d; i += 4) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        acc0 = vfmaq_f32(acc0, d0, d0);
    }
    float sum = vaddvq_f32(acc0) + vaddvq_f32(acc1);
    for (; i < d; ++i) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

float norm_sq_f32_neon(const float* a, int d) {
    return dot_f32_neon(a, a, d);
}
#endif

KernelDispatchTable build_scalar_table() {
    KernelDispatchTable t{};
#define SET_ROW(idx, fn) \
    t.numeric[idx][OP_GT]  = &fn<OP_GT>;  t.numeric[idx][OP_LT]  = &fn<OP_LT>; \
    t.numeric[idx][OP_GTE] = &fn<OP_GTE>; t.numeric[idx][OP_LTE] = &fn<OP_LTE>; \
    t.numeric[idx][OP_EQ]  = &fn<OP_EQ>;  t.numeric[idx][OP_NEQ] = &fn<OP_NEQ>;
    SET_ROW(0, eval_f32_s)
    SET_ROW(1, eval_f64_s)
    SET_ROW(2, eval_i32_s)
    SET_ROW(3, eval_i64_s)
#undef SET_ROW
    t.range[0][0] = &eval_range<float, true>;    t.range[0][1] = &eval_range<float, false>;
    t.range[1][0] = &eval_range<double, true>;   t.range[1][1] = &eval_range<double, false>;
    t.range[2][0] = &eval_range<int32_t, true>;  t.range[2][1] = &eval_range<int32_t, false>;
    t.range[3][0] = &eval_range<int64_t, true>;  t.range[3][1] = &eval_range<int64_t, false>;
    t.string_[0] = &eval_in_dict_scalar;
    t.string_[1] = &eval_not_in_dict_scalar;
    t.string_[2] = &eval_eq_dict_scalar;
    t.bitwise_and = &bitwise_and_scalar;
    t.bitwise_or = &bitwise_or_scalar;
    t.bitwise_not = &bitwise_not_scalar;
    t.dot_f32 = &dot_f32_scalar;
    t.l2sq_f32 = &l2sq_f32_scalar;
    t.norm_sq_f32 = &norm_sq_f32_scalar;
    return t;
}

KernelDispatchTable build_compiled_table() {
    KernelDispatchTable t{};
#if BLAZERULES_NEON
#define SET_ROW(idx, fn) \
    t.numeric[idx][OP_GT]  = &fn<OP_GT>;  t.numeric[idx][OP_LT]  = &fn<OP_LT>; \
    t.numeric[idx][OP_GTE] = &fn<OP_GTE>; t.numeric[idx][OP_LTE] = &fn<OP_LTE>; \
    t.numeric[idx][OP_EQ]  = &fn<OP_EQ>;  t.numeric[idx][OP_NEQ] = &fn<OP_NEQ>;
    SET_ROW(0, eval_f32_n)
    SET_ROW(1, eval_f64_n)
    SET_ROW(2, eval_i32_n)
    SET_ROW(3, eval_i64_n)
#undef SET_ROW
#else
#define SET_ROW(idx, fn) \
    t.numeric[idx][OP_GT]  = &fn<OP_GT>;  t.numeric[idx][OP_LT]  = &fn<OP_LT>; \
    t.numeric[idx][OP_GTE] = &fn<OP_GTE>; t.numeric[idx][OP_LTE] = &fn<OP_LTE>; \
    t.numeric[idx][OP_EQ]  = &fn<OP_EQ>;  t.numeric[idx][OP_NEQ] = &fn<OP_NEQ>;
    SET_ROW(0, eval_f32_s)
    SET_ROW(1, eval_f64_s)
    SET_ROW(2, eval_i32_s)
    SET_ROW(3, eval_i64_s)
#undef SET_ROW
#endif
    t.range[0][0] = &eval_range<float, true>;    t.range[0][1] = &eval_range<float, false>;
    t.range[1][0] = &eval_range<double, true>;   t.range[1][1] = &eval_range<double, false>;
    t.range[2][0] = &eval_range<int32_t, true>;  t.range[2][1] = &eval_range<int32_t, false>;
    t.range[3][0] = &eval_range<int64_t, true>;  t.range[3][1] = &eval_range<int64_t, false>;

    t.string_[0] = &eval_in_dict;
    t.string_[1] = &eval_not_in_dict;
    t.string_[2] = &eval_eq_dict;
    t.bitwise_and = &bitwise_and_kernel;
    t.bitwise_or = &bitwise_or_kernel;
    t.bitwise_not = &bitwise_not_kernel;
#if BLAZERULES_NEON
    t.dot_f32 = &dot_f32_neon;
    t.l2sq_f32 = &l2sq_f32_neon;
    t.norm_sq_f32 = &norm_sq_f32_neon;
#else
    t.dot_f32 = &dot_f32_scalar;
    t.l2sq_f32 = &l2sq_f32_scalar;
    t.norm_sq_f32 = &norm_sq_f32_scalar;
#endif
    return t;
}

CpuFeatures detect_cpu_features() {
    CpuFeatures f;
#if defined(__APPLE__)
    f.os = "macos";
#elif defined(_WIN32)
    f.os = "windows";
#elif defined(__linux__)
    f.os = "linux";
#else
    f.os = "unknown";
#endif
#if defined(__clang__)
    f.compiler = "clang";
#elif defined(__GNUC__)
    f.compiler = "gcc";
#elif defined(_MSC_VER)
    f.compiler = "msvc";
#else
    f.compiler = "unknown";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    f.arm64 = true;
    f.neon = true;
    f.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__x86_64__) || defined(_M_X64)
    f.x86_64 = true;
#else
    f.x86_64 = false;
#endif
    f.arch = f.x86_64 ? "x86_64" : "x86";
#if defined(_MSC_VER)
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    int max_leaf = regs[0];
    if (max_leaf >= 1) {
        __cpuidex(regs, 1, 0);
        f.sse2 = (regs[3] & (1 << 26)) != 0;
        f.popcnt = (regs[2] & (1 << 23)) != 0;
        f.avx = (regs[2] & (1 << 28)) != 0;
        f.fma = (regs[2] & (1 << 12)) != 0;
        bool osxsave = (regs[2] & (1 << 27)) != 0;
        if (f.avx && osxsave) {
            unsigned long long xcr0 = _xgetbv(0);
            f.os_avx = (xcr0 & 0x6) == 0x6;
            f.os_avx512 = (xcr0 & 0xe6) == 0xe6;
        }
    }
    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        f.avx2 = (regs[1] & (1 << 5)) != 0;
        f.bmi2 = (regs[1] & (1 << 8)) != 0;
        f.avx512f = (regs[1] & (1 << 16)) != 0;
        f.avx512dq = (regs[1] & (1 << 17)) != 0;
        f.avx512bw = (regs[1] & (1 << 30)) != 0;
        f.avx512vl = (regs[1] & (1 << 31)) != 0;
        f.avx512vpopcntdq = (regs[2] & (1 << 14)) != 0;
    }
    __cpuid(regs, 0x80000000);
    int max_ext = regs[0];
    if (max_ext >= 0x80000001) {
        __cpuidex(regs, 0x80000001, 0);
        f.lzcnt = (regs[2] & (1 << 5)) != 0;
    }
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    f.sse2 = __builtin_cpu_supports("sse2");
    f.avx = __builtin_cpu_supports("avx");
    f.avx2 = __builtin_cpu_supports("avx2");
    f.fma = __builtin_cpu_supports("fma");
    f.bmi2 = __builtin_cpu_supports("bmi2");
    f.popcnt = __builtin_cpu_supports("popcnt");
#if defined(__GNUC__) && !defined(__clang__)
    f.lzcnt = __builtin_cpu_supports("lzcnt");
#else
    f.lzcnt = false;
#endif
    f.avx512f = __builtin_cpu_supports("avx512f");
    f.avx512bw = __builtin_cpu_supports("avx512bw");
    f.avx512vl = __builtin_cpu_supports("avx512vl");
    f.avx512dq = __builtin_cpu_supports("avx512dq");
#if defined(__GNUC__) && !defined(__clang__)
    f.avx512vpopcntdq = __builtin_cpu_supports("avx512vpopcntdq");
#else
    f.avx512vpopcntdq = false;
#endif
    f.os_avx = f.avx;
    f.os_avx512 = f.avx512f;
#endif
#endif
#else
    f.arch = "unknown";
#endif
    return f;
}

bool compiled_backend_available(SimdBackend backend) {
    switch (backend) {
        case SimdBackend::SCALAR:
            return true;
        case SimdBackend::NEON:
            return BLAZERULES_NEON;
        case SimdBackend::SSE2:
            return true;  // The scalar table is safe on SSE2-only x86_64 hosts.
        case SimdBackend::AVX2:
#if defined(BLAZERULES_BUILD_AVX2)
            return true;
#else
            return false;
#endif
        case SimdBackend::AVX512:
#if defined(BLAZERULES_BUILD_AVX512)
            return true;
#else
            return false;
#endif
    }
    return false;
}

SimdBackend parse_backend(std::string_view name, bool& ok) {
    ok = true;
    if (name == "auto" || name.empty()) {
        ok = false;
        return SimdBackend::SCALAR;
    }
    if (name == "scalar") return SimdBackend::SCALAR;
    if (name == "neon") return SimdBackend::NEON;
    if (name == "sse2") return SimdBackend::SSE2;
    if (name == "avx2") return SimdBackend::AVX2;
    if (name == "avx512") return SimdBackend::AVX512;
    ok = false;
    return SimdBackend::SCALAR;
}

bool backend_supported_now(SimdBackend backend) {
    const CpuFeatures& f = cpu_features();
    if (!compiled_backend_available(backend)) return false;
    switch (backend) {
        case SimdBackend::SCALAR:
            return true;
        case SimdBackend::NEON:
            return f.neon;
        case SimdBackend::SSE2:
            return f.sse2 || f.x86_64;
        case SimdBackend::AVX2:
            return f.x86_64 && f.os_avx && f.avx2 && f.fma;
        case SimdBackend::AVX512:
            return f.x86_64 && f.os_avx512 && f.avx512f && f.avx512bw &&
                   f.avx512vl && f.avx512dq;
    }
    return false;
}

SimdBackend auto_backend() {
    const CpuFeatures& f = cpu_features();
    if (g_enable_avx512_auto.load(std::memory_order_relaxed) &&
        backend_supported_now(SimdBackend::AVX512)) {
        return SimdBackend::AVX512;
    }
    if (backend_supported_now(SimdBackend::AVX2)) return SimdBackend::AVX2;
    if (f.neon && backend_supported_now(SimdBackend::NEON)) return SimdBackend::NEON;
    return SimdBackend::SCALAR;
}

SimdBackend selected_backend() {
    int forced = g_forced_backend.load(std::memory_order_relaxed);
    if (forced != BACKEND_AUTO) return static_cast<SimdBackend>(forced);
    const char* env = std::getenv("BLAZERULES_SIMD_BACKEND");
    if (env && *env) {
        bool ok = false;
        SimdBackend requested = parse_backend(env, ok);
        if (ok && backend_supported_now(requested)) return requested;
    }
    return auto_backend();
}

const KernelDispatchTable& table_for_backend(SimdBackend backend) {
    static const KernelDispatchTable scalar = build_scalar_table();
    static const KernelDispatchTable compiled = build_compiled_table();
    switch (backend) {
        case SimdBackend::NEON:
            return compiled;
        case SimdBackend::AVX2:
#if defined(BLAZERULES_BUILD_AVX2)
            return avx2_kernel_table();
#else
            return scalar;
#endif
        case SimdBackend::AVX512:
#if defined(BLAZERULES_BUILD_AVX512)
            return avx512_kernel_table();
#elif defined(BLAZERULES_BUILD_AVX2)
            return avx2_kernel_table();
#else
            return scalar;
#endif
        case SimdBackend::SSE2:
        case SimdBackend::SCALAR:
        default:
            return scalar;
    }
}

} // namespace

const CpuFeatures& cpu_features() {
    static const CpuFeatures features = detect_cpu_features();
    return features;
}

bool simd_backend_supported(SimdBackend backend) {
    return backend_supported_now(backend);
}

const char* simd_backend_name(SimdBackend backend) {
    switch (backend) {
        case SimdBackend::SCALAR: return "scalar";
        case SimdBackend::NEON: return "neon";
        case SimdBackend::SSE2: return "sse2";
        case SimdBackend::AVX2: return "avx2";
        case SimdBackend::AVX512: return "avx512";
    }
    return "unknown";
}

SimdBackend simd_backend() {
    return selected_backend();
}

const char* simd_backend_name() {
    return simd_backend_name(simd_backend());
}

std::string cpu_features_summary() {
    const CpuFeatures& f = cpu_features();
    std::ostringstream out;
    out << "arch=" << f.arch
        << ",os=" << f.os
        << ",compiler=" << f.compiler
        << ",backend=" << simd_backend_name()
        << ",neon=" << (f.neon ? 1 : 0)
        << ",sse2=" << (f.sse2 ? 1 : 0)
        << ",avx=" << (f.avx ? 1 : 0)
        << ",avx2=" << (f.avx2 ? 1 : 0)
        << ",fma=" << (f.fma ? 1 : 0)
        << ",bmi2=" << (f.bmi2 ? 1 : 0)
        << ",popcnt=" << (f.popcnt ? 1 : 0)
        << ",lzcnt=" << (f.lzcnt ? 1 : 0)
        << ",avx512f=" << (f.avx512f ? 1 : 0)
        << ",avx512bw=" << (f.avx512bw ? 1 : 0)
        << ",avx512vl=" << (f.avx512vl ? 1 : 0)
        << ",avx512dq=" << (f.avx512dq ? 1 : 0)
        << ",os_avx=" << (f.os_avx ? 1 : 0)
        << ",os_avx512=" << (f.os_avx512 ? 1 : 0);
    return out.str();
}

bool set_simd_backend_override(std::string_view backend,
                               bool enable_avx512_auto,
                               std::string* error) {
    g_enable_avx512_auto.store(enable_avx512_auto, std::memory_order_relaxed);
    if (backend.empty() || backend == "auto") {
        g_forced_backend.store(BACKEND_AUTO, std::memory_order_relaxed);
        return true;
    }
    bool ok = false;
    SimdBackend requested = parse_backend(backend, ok);
    if (!ok) {
        if (error) *error = "simd_backend_override must be auto, scalar, neon, sse2, avx2, or avx512";
        return false;
    }
    if (!backend_supported_now(requested)) {
        if (error) {
            *error = std::string("requested SIMD backend is unavailable on this build/CPU: ") +
                     simd_backend_name(requested);
        }
        return false;
    }
    g_forced_backend.store(static_cast<int>(requested), std::memory_order_relaxed);
    return true;
}

void reset_simd_backend_override() {
    g_forced_backend.store(BACKEND_AUTO, std::memory_order_relaxed);
    g_enable_avx512_auto.store(false, std::memory_order_relaxed);
}

const KernelDispatchTable& kernels() {
    return table_for_backend(selected_backend());
}

bool simd_enabled() {
    SimdBackend b = simd_backend();
    return b != SimdBackend::SCALAR && b != SimdBackend::SSE2;
}
