#ifndef BLAZERULES_SIMD_VEC_H
#define BLAZERULES_SIMD_VEC_H

#include <cmath>
#include <cstddef>

#include "blazerules/simd_kernels.h"

namespace blazerules {

inline float dot_f32(const float* a, const float* b, int d) {
    return kernels().dot_f32(a, b, d);
}

inline float l2sq_f32(const float* a, const float* b, int d) {
    return kernels().l2sq_f32(a, b, d);
}

inline float norm_sq_f32(const float* a, int d) { return kernels().norm_sq_f32(a, d); }

inline float cosine_f32(const float* x, const float* ref, int d, float ref_inv_norm) {
    float dotxr = dot_f32(x, ref, d);
    float nx = norm_sq_f32(x, d);
    if (nx <= 0.0f) return 0.0f;
    return dotxr * ref_inv_norm / std::sqrt(nx);
}

} // namespace blazerules

#endif // BLAZERULES_SIMD_VEC_H
