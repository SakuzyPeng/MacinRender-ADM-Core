// 跨平台数值一致性的回归守卫：方向向量长度必须逐位可复现。
//
// 背景见 docs/architecture/CONSISTENCY_LOCALIZATION.md。首轮三平台基线里 `hoa-hoa3-point`
// 的分歧就定位在这一步：`hoa.02-direction`（未归一化方向）三平台相同，`hoa.03-normalized`
// 不同。原因是 `std::hypot` 的精度没有标准约束，Darwin 对同一个单位向量返回恰好 1.0，
// glibc / UCRT 返回 1-1ulp，除完之后每个分量差 1 ULP。
//
// 本测试在本地 CTest 和三平台 consistency workflow 的 Release A/B 构建中执行。
// 期望值按固定求和顺序的 IEEE-754 规则算出；最终渲染仍由 PCM 位比较验收。

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
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::uint32_t expected{};
    std::string_view what{};
};

// 期望值由 IEEE-754 各步的规定推出（float 的平方在 double 中精确 → double 求和正确舍入
// → sqrt 正确舍入 → 窄化正确舍入），不是从某一台机器的输出抄回来的。输入直接给位模式，
// 避免把 cosf / sinf 的平台差异带进测试本身。
constexpr std::array<LengthCase, 8> k_length_cases{{
    {0x3F5DB3D7U, 0x3F000000U, 0x00000000U, 0x3F800000U, "az=30 el=0 的方向：fixture 用的那个，hypot 在此处分歧"},
    {0xBEAF1D44U, 0x3F708FB2U, 0x00000000U, 0x3F800000U, "az=110 el=0"},
    {0x00000000U, 0x3F800000U, 0x00000000U, 0x3F800000U, "单位轴 +Y"},
    {0x40400000U, 0x40800000U, 0x00000000U, 0x40A00000U, "3,4,0：长度恰好 5"},
    {0x40000000U, 0x40400000U, 0x40C00000U, 0x40E00000U, "2,3,6：三个非零分量，长度恰好 7"},
    {0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, "零向量"},
    {0x7E967699U, 0x7E967699U, 0x00000000U, 0x7ED4C986U, "接近 float 上限：double 累加不会溢出"},
    {0x00000001U, 0x00000002U, 0x00000000U, 0x00000002U, "次正规分量：double 累加不会下溢成零"},
}};

// 即使输入表是 constexpr，也要在 Release 下执行实际浮点运算，避免常量折叠替代被测路径。
float runtime_float(std::uint32_t bits) {
    const volatile std::uint32_t runtime_bits = bits;
    return std::bit_cast<float>(static_cast<std::uint32_t>(runtime_bits));
}

void check_bits(std::uint32_t actual, std::uint32_t expected, std::string_view what, int& failures) {
    if (actual != expected) {
        std::cerr << "FAIL " << what << ": expected 0x" << std::hex << expected << " got 0x" << actual << std::dec
                  << "\n";
        ++failures;
    }
}

void test_canonical_lengths(int& failures) {
    for (const auto& c : k_length_cases) {
        const float length = canonical_vector_length(runtime_float(c.x), runtime_float(c.y), runtime_float(c.z));
        check_bits(std::bit_cast<std::uint32_t>(length), c.expected, c.what, failures);
    }
}

// 三项非零、量级不同，能区分两次加法的舍入顺序。精确平方后，(x²+y²)+z² 的
// binary64 位模式为 0x3ff0000020000012，(z²+y²)+x² 为 0x3ff0000020000011。
// sqrt 再窄化到 binary32 后落在舍入边界两侧；这不是分量置换不变性的承诺。
void test_fixed_sum_order(int& failures) {
    const float x = runtime_float(0x3F800000U);
    const float y = runtime_float(0x39B5016CU);
    const float z = runtime_float(0x368EF881U);
    check_bits(std::bit_cast<std::uint32_t>(canonical_vector_length(x, y, z)),
               0x3F800001U,
               "xyz：先相加前两项，再加第三项",
               failures);
    check_bits(std::bit_cast<std::uint32_t>(canonical_vector_length(z, y, x)),
               0x3F800000U,
               "zyx：同样的求和规则在分量置换后有不同的舍入结果",
               failures);
}

// 由 cos/sin 构造的方向若长度恰好是 1.0f，归一化必须原样返回。hypot 返回 1-1ulp 时不成立
// ——那正是 `hoa.03-normalized` 被推偏一个 ULP 的机制。
void test_unit_vector_is_not_perturbed(int& failures) {
    const float x = runtime_float(0x3F5DB3D7U);
    const float y = runtime_float(0x3F000000U);
    const float len = canonical_vector_length(x, y, 0.0F);
    check_bits(std::bit_cast<std::uint32_t>(len), 0x3F800000U, "单位向量长度应为恰好 1.0f", failures);
    check_bits(std::bit_cast<std::uint32_t>(x / len), 0x3F5DB3D7U, "归一化不应改动 x", failures);
    check_bits(std::bit_cast<std::uint32_t>(y / len), 0x3F000000U, "归一化不应改动 y", failures);
}

} // namespace

int main() {
    int failures = 0;
    test_canonical_lengths(failures);
    test_fixed_sum_order(failures);
    test_unit_vector_is_not_perturbed(failures);

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "render_common numeric checks passed\n";
    return EXIT_SUCCESS;
}
