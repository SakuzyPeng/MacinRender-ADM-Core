// 跨平台数值一致性的回归守卫：方向向量长度必须逐位可复现。
//
// 背景见 docs/architecture/CONSISTENCY_LOCALIZATION.md。首轮三平台基线里 `hoa-hoa3-point`
// 的分歧就定位在这一步：`hoa.02-direction`（未归一化方向）三平台相同，`hoa.03-normalized`
// 不同。原因是 `std::hypot` 的精度没有标准约束，Darwin 对同一个单位向量返回恰好 1.0，
// glibc / UCRT 返回 1-1ulp，除完之后每个分量差 1 ULP。
//
// 三平台 consistency workflow 才是最终验收，但它只在 push main 时跑。本测试把同一个约束
// 拉到本地 CTest：期望值按 IEEE-754 规则独立算出，任一平台算不出这些位模式即失败。

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "render_common.h"

namespace {

using mradm::render_common::canonical_vector_length;

struct LengthCase {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;
    std::uint32_t expected;
    std::string_view what;
};

// 期望值由 IEEE-754 各步的规定推出（float 的平方在 double 中精确 → double 求和正确舍入
// → sqrt 正确舍入 → 窄化正确舍入），不是从某一台机器的输出抄回来的。输入直接给位模式，
// 避免把 cosf / sinf 的平台差异带进测试本身。
constexpr std::array<LengthCase, 7> k_length_cases{{
    {0x3F5DB3D7U, 0x3F000000U, 0x00000000U, 0x3F800000U, "az=30 el=0 的方向：fixture 用的那个，hypot 在此处分歧"},
    {0xBEAF1D44U, 0x3F708FB2U, 0x00000000U, 0x3F800000U, "az=110 el=0"},
    {0x00000000U, 0x3F800000U, 0x00000000U, 0x3F800000U, "单位轴 +Y"},
    {0x40400000U, 0x40800000U, 0x00000000U, 0x40A00000U, "3,4,0：长度恰好 5"},
    {0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, "零向量"},
    {0x7E967699U, 0x7E967699U, 0x00000000U, 0x7ED4C986U, "接近 float 上限：double 累加不会溢出"},
    {0x00000001U, 0x00000002U, 0x00000000U, 0x00000002U, "次正规分量：double 累加不会下溢成零"},
}};

int failures = 0;

void check_bits(std::uint32_t actual, std::uint32_t expected, std::string_view what) {
    if (actual != expected) {
        std::cerr << "FAIL " << what << ": expected 0x" << std::hex << expected << " got 0x" << actual << std::dec
                  << "\n";
        ++failures;
    }
}

void test_canonical_lengths() {
    for (const auto& c : k_length_cases) {
        const float length =
            canonical_vector_length(std::bit_cast<float>(c.x), std::bit_cast<float>(c.y), std::bit_cast<float>(c.z));
        check_bits(std::bit_cast<std::uint32_t>(length), c.expected, c.what);
    }
}

// 分量顺序不影响结果：平方在 double 里精确，两次加法的舍入对这三项是对称的。
// 如果哪天有人把实现改成 float 累加，这条会先炸。
void test_component_order_is_irrelevant() {
    for (const auto& c : k_length_cases) {
        const float x = std::bit_cast<float>(c.x);
        const float y = std::bit_cast<float>(c.y);
        const float z = std::bit_cast<float>(c.z);
        const auto base = std::bit_cast<std::uint32_t>(canonical_vector_length(x, y, z));
        check_bits(std::bit_cast<std::uint32_t>(canonical_vector_length(z, y, x)), base, c.what);
        check_bits(std::bit_cast<std::uint32_t>(canonical_vector_length(y, x, z)), base, c.what);
    }
}

// 由 cos/sin 构造的方向若长度恰好是 1.0f，归一化必须原样返回。hypot 返回 1-1ulp 时不成立
// ——那正是 `hoa.03-normalized` 被推偏一个 ULP 的机制。
void test_unit_vector_is_not_perturbed() {
    const float x = std::bit_cast<float>(0x3F5DB3D7U);
    const float y = std::bit_cast<float>(0x3F000000U);
    const float len = canonical_vector_length(x, y, 0.0F);
    check_bits(std::bit_cast<std::uint32_t>(len), 0x3F800000U, "单位向量长度应为恰好 1.0f");
    check_bits(std::bit_cast<std::uint32_t>(x / len), 0x3F5DB3D7U, "归一化不应改动 x");
    check_bits(std::bit_cast<std::uint32_t>(y / len), 0x3F000000U, "归一化不应改动 y");
}

} // namespace

int main() {
    test_canonical_lengths();
    test_component_order_is_irrelevant();
    test_unit_vector_is_not_perturbed();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "render_common numeric checks passed\n";
    return EXIT_SUCCESS;
}
