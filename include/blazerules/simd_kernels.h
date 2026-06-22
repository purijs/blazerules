#ifndef BLAZERULES_SIMD_KERNELS_H
#define BLAZERULES_SIMD_KERNELS_H

#include <cstdint>
#include <string>
#include <string_view>

enum class SimdBackend {
    SCALAR = 0,
    NEON = 1,
    SSE2 = 2,
    AVX2 = 3,
    AVX512 = 4,
};

struct CpuFeatures {
    bool x86_64 = false;
    bool arm64 = false;
    bool sse2 = false;
    bool avx = false;
    bool avx2 = false;
    bool avx512f = false;
    bool avx512bw = false;
    bool avx512vl = false;
    bool avx512dq = false;
    bool avx512vpopcntdq = false;
    bool fma = false;
    bool bmi2 = false;
    bool popcnt = false;
    bool lzcnt = false;
    bool neon = false;
    bool os_avx = false;
    bool os_avx512 = false;
    std::string arch;
    std::string compiler;
    std::string os;
};

struct KernelDispatchTable {
    using NumericKernelFn = void (*)(const void*, const uint8_t*, double, uint8_t*, int);
    using RangeKernelFn = void (*)(const void*, const uint8_t*, double, double, uint8_t*, int);
    using StringKernelFn = void (*)(const int32_t*, const uint8_t*, const int32_t*, int, uint8_t*, int);
    using BitwiseKernelFn = void (*)(const uint8_t* const*, int, uint8_t*, int);
    using BitwiseNotKernelFn = void (*)(const uint8_t*, uint8_t*, int, int);
    using VectorBinaryF32Fn = float (*)(const float*, const float*, int);
    using VectorUnaryF32Fn = float (*)(const float*, int);

    NumericKernelFn numeric[4][6]{};
    RangeKernelFn range[4][2]{};
    StringKernelFn string_[3]{};
    BitwiseKernelFn bitwise_and = nullptr;
    BitwiseKernelFn bitwise_or = nullptr;
    BitwiseNotKernelFn bitwise_not = nullptr;
    VectorBinaryF32Fn dot_f32 = nullptr;
    VectorBinaryF32Fn l2sq_f32 = nullptr;
    VectorUnaryF32Fn norm_sq_f32 = nullptr;
};

const KernelDispatchTable& kernels();
bool simd_enabled();
SimdBackend simd_backend();
const char* simd_backend_name();
const CpuFeatures& cpu_features();
std::string cpu_features_summary();
bool set_simd_backend_override(std::string_view backend,
                               bool enable_avx512_auto = false,
                               std::string* error = nullptr);
void reset_simd_backend_override();
bool simd_backend_supported(SimdBackend backend);
const char* simd_backend_name(SimdBackend backend);

#endif // BLAZERULES_SIMD_KERNELS_H
