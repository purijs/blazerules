#ifndef BLAZERULES_BITMASK_H
#define BLAZERULES_BITMASK_H

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace blazerules {

inline int popcount32(uint32_t value) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt(value));
#else
    return __builtin_popcount(value);
#endif
}

inline int popcount64(uint64_t value) {
#if defined(_MSC_VER) && defined(_M_X64)
    return static_cast<int>(__popcnt64(value));
#elif defined(_MSC_VER)
    return popcount32(static_cast<uint32_t>(value)) +
           popcount32(static_cast<uint32_t>(value >> 32));
#else
    return __builtin_popcountll(value);
#endif
}

inline int ctz32(uint32_t value) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward(&index, value);
    return static_cast<int>(index);
#else
    return __builtin_ctz(value);
#endif
}

inline void prefetch_read(const void* ptr) {
#if defined(_MSC_VER)
    _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 0, 3);
#else
    (void)ptr;
#endif
}

inline int count_set_bits(const uint8_t* bitmask, int bitmask_bytes) {
    int count = 0, i = 0;
#if defined(__aarch64__)
    for (; i + 16 <= bitmask_bytes; i += 16) {
        uint8x16_t bytes = vld1q_u8(bitmask + i);
        uint8x16_t bits_per_byte = vcntq_u8(bytes);
        count += vaddvq_u8(bits_per_byte);
    }
#endif
    for (; i + 8 <= bitmask_bytes; i += 8) {
        uint64_t word;
        std::memcpy(&word, bitmask + i, 8);
        count += popcount64(word);
    }
    for (; i < bitmask_bytes; ++i) count += popcount32(bitmask[i]);
    return count;
}

inline void find_set_bits(const uint8_t* bitmask, int n_records,
                          std::vector<int32_t>& out_indices) {
    int bitmask_bytes = (n_records + 7) / 8;
    int b = 0;
    for (; b + 4 <= bitmask_bytes; b += 4) {
        uint32_t word;
        std::memcpy(&word, bitmask + b, 4);
        while (word != 0) {
            int bit = ctz32(word);
            int idx = b * 8 + bit;
            if (idx < n_records) out_indices.push_back(idx);
            word &= word - 1;
        }
    }
    for (; b < bitmask_bytes; ++b) {
        uint8_t byte = bitmask[b];
        while (byte != 0) {
            int bit = ctz32(byte);
            int idx = b * 8 + bit;
            if (idx < n_records) out_indices.push_back(idx);
            byte &= byte - 1;
        }
    }
}

inline void or_into(uint8_t* dst, const uint8_t* src, int bitmask_bytes) {
    int i = 0;
#if defined(__aarch64__)
    for (; i + 16 <= bitmask_bytes; i += 16) {
        uint8x16_t a = vld1q_u8(dst + i);
        uint8x16_t b = vld1q_u8(src + i);
        vst1q_u8(dst + i, vorrq_u8(a, b));
    }
#endif
    for (; i + 8 <= bitmask_bytes; i += 8) {
        uint64_t a, b;
        std::memcpy(&a, dst + i, 8);
        std::memcpy(&b, src + i, 8);
        a |= b;
        std::memcpy(dst + i, &a, 8);
    }
    for (; i < bitmask_bytes; ++i) dst[i] |= src[i];
}

} // namespace blazerules

#endif //BLAZERULES_BITMASK_H
