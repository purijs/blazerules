#include <gtest/gtest.h>

#include "blazerules/simd_kernels.h"

#include <array>
#include <cstdint>

namespace {

bool bit_is_set(const uint8_t* bits, int row) {
    return ((bits[row >> 3] >> (row & 7)) & 1u) != 0;
}

}  // namespace

TEST(KernelTest, NumericFloatGtSetsMaskAndZeroesTail) {
    std::array<float, 10> values{1.0f, 5.0f, 8.0f, 2.0f, 9.0f, 3.0f, 7.0f, 4.0f, 6.0f, 0.0f};
    std::array<uint8_t, 2> out{0xff, 0xff};

    kernels().numeric[0][0](values.data(), nullptr, 4.0, out.data(), static_cast<int>(values.size()));

    const bool expected[] = {false, true, true, false, true, false, true, false, true, false};
    for (int i = 0; i < 10; ++i) EXPECT_EQ(bit_is_set(out.data(), i), expected[i]) << i;
    EXPECT_EQ(out[1] & 0b11111100, 0) << "tail bits beyond row count must be cleared";
}

TEST(KernelTest, NumericInt32EqHonorsValidity) {
    std::array<int32_t, 8> values{7, 7, 3, 7, 2, 7, 7, 9};
    std::array<uint8_t, 1> validity{0b11010111};  // row 3 and 5 are null
    std::array<uint8_t, 1> out{0xff};

    kernels().numeric[2][4](values.data(), validity.data(), 7.0, out.data(), static_cast<int>(values.size()));

    const bool expected[] = {true, true, false, false, false, false, true, false};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(bit_is_set(out.data(), i), expected[i]) << i;
}

TEST(KernelTest, BitwiseAndOrNotWorkOnMasks) {
    std::array<uint8_t, 2> a{0b10101100, 0b00000011};
    std::array<uint8_t, 2> b{0b11110000, 0b00000001};
    std::array<uint8_t, 2> out{};
    const uint8_t* inputs[] = {a.data(), b.data()};

    // bitwise_and/or fold each input INTO the existing `out` buffer, matching the
    // engine's contract (it seeds the accumulator — fill_true() for AND, the first
    // child's mask for OR — then folds the remaining inputs). Seed accordingly here.
    out.fill(0xFF);
    kernels().bitwise_and(inputs, 2, out.data(), 2);
    EXPECT_EQ(out[0], 0b10100000);
    EXPECT_EQ(out[1], 0b00000001);

    out.fill(0x00);
    kernels().bitwise_or(inputs, 2, out.data(), 2);
    EXPECT_EQ(out[0], 0b11111100);
    EXPECT_EQ(out[1], 0b00000011);

    kernels().bitwise_not(a.data(), out.data(), 2, 10);
    EXPECT_EQ(out[0], static_cast<uint8_t>(~a[0]));
    EXPECT_EQ(out[1] & 0b00000011, 0b00000000);
    EXPECT_EQ(out[1] & 0b11111100, 0);
}
