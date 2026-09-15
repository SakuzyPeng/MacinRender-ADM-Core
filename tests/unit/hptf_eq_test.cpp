// HpTF(AutoEq ParametricEQ)解析 / 系数设计 / 级联运行 / 热切换的单元测试。
// 全部 fixture 都是字符串字面量与程序生成的信号,不依赖任何私有音频素材。

#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "hptf_eq.h"

namespace {

using mradm::render_common::cascade_magnitude_db;
using mradm::render_common::design_cascade;
using mradm::render_common::HptfBandType;
using mradm::render_common::HptfCascade;
using mradm::render_common::HptfCoefficients;
using mradm::render_common::HptfPreampMode;
using mradm::render_common::HptfProcessor;
using mradm::render_common::k_hptf_blend_frames;
using mradm::render_common::parse_parametric_eq;

bool check(bool cond, std::string_view msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return cond;
}

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

constexpr std::uint32_t k_fs = 48000;

// AutoEq: results/oratory1990/over-ear/Sony MDR-MV1/Sony MDR-MV1 ParametricEQ.txt
constexpr std::string_view k_mv1 = R"(Preamp: -4.1 dB
Filter 1: ON LSC Fc 105 Hz Gain 9.7 dB Q 0.70
Filter 2: ON PK Fc 46 Hz Gain -9.2 dB Q 0.37
Filter 3: ON PK Fc 1722 Hz Gain 2.8 dB Q 0.95
Filter 4: ON PK Fc 4643 Hz Gain 9.2 dB Q 1.99
Filter 5: ON PK Fc 5779 Hz Gain -9.2 dB Q 1.50
Filter 6: ON HSC Fc 10000 Hz Gain -2.2 dB Q 0.70
Filter 7: ON PK Fc 705 Hz Gain 0.8 dB Q 2.45
Filter 8: ON PK Fc 242 Hz Gain -0.7 dB Q 2.04
Filter 9: ON PK Fc 128 Hz Gain 0.5 dB Q 2.09
Filter 10: ON PK Fc 2955 Hz Gain -0.9 dB Q 4.58
)";

HptfCoefficients design_mv1(HptfPreampMode mode = HptfPreampMode::warn_only) {
    auto profile = parse_parametric_eq(k_mv1);
    auto coeffs = design_cascade(*profile, k_fs, mode);
    return *coeffs;
}

// 冲激响应在 hz 处的幅度(dB)。用朴素 DFT 单点求值,不引入 FFT 依赖。
double impulse_response_db(const HptfCoefficients& coeffs, double hz, std::size_t taps = 32768) {
    HptfCascade cascade;
    cascade.prepare(1);
    cascade.set_coefficients(coeffs);
    std::vector<float> h(taps, 0.0F);
    h[0] = 1.0F;
    cascade.process(h.data(), taps);

    const double w = 2.0 * std::acos(-1.0) * hz / static_cast<double>(k_fs);
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = 0; n < taps; ++n) {
        if (h[n] == 0.0F) {
            continue;
        }
        const double ang = -w * static_cast<double>(n);
        acc += static_cast<double>(h[n]) * std::complex<double>{std::cos(ang), std::sin(ang)};
    }
    return 20.0 * std::log10(std::max(std::abs(acc), 1e-30));
}

// ── (a) 解析 ──────────────────────────────────────────────────────────────────

bool test_parse() {
    bool ok = true;

    auto profile = parse_parametric_eq(k_mv1);
    if (!check(profile.has_value(), "parse: MDR-MV1 解析成功")) {
        return false;
    }
    ok &= check(near(profile->preamp_db, -4.1, 1e-9), "parse: preamp = -4.1 dB");
    ok &= check(profile->bands.size() == 10, "parse: 10 段");
    ok &= check(profile->bands[0].type == HptfBandType::low_shelf, "parse: 第 1 段是 LSC");
    ok &= check(near(profile->bands[0].fc_hz, 105.0, 1e-9), "parse: 第 1 段 Fc=105");
    ok &= check(near(profile->bands[0].gain_db, 9.7, 1e-9), "parse: 第 1 段 Gain=9.7");
    ok &= check(near(profile->bands[0].q, 0.70, 1e-9), "parse: 第 1 段 Q=0.70");
    ok &= check(profile->bands[5].type == HptfBandType::high_shelf, "parse: 第 6 段是 HSC");
    ok &= check(profile->bands[1].type == HptfBandType::peaking, "parse: 第 2 段是 PK");
    ok &= check(near(profile->bands[1].gain_db, -9.2, 1e-9), "parse: 负增益解析正确");

    // OFF 行保留但不计入启用段
    auto with_off = parse_parametric_eq("Preamp: 0 dB\n"
                                        "Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1\n"
                                        "Filter 2: OFF PK Fc 2000 Hz Gain 3 dB Q 1\n");
    ok &= check(with_off.has_value() && with_off->bands.size() == 2, "parse: OFF 行保留");
    ok &= check(with_off.has_value() && with_off->bands[1].enabled == false, "parse: OFF 行 enabled=false");
    if (with_off.has_value()) {
        auto c = design_cascade(*with_off, k_fs, HptfPreampMode::warn_only);
        ok &= check(c.has_value() && c->band_count == 1, "design: OFF 段不进级联");
    }

    // 缺 Preamp 行 → 0 dB
    auto no_preamp = parse_parametric_eq("Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1\n");
    ok &= check(no_preamp.has_value() && near(no_preamp->preamp_db, 0.0, 1e-12), "parse: 缺 Preamp 按 0 dB");

    // CRLF / 空行 / 注释 / 前后空白
    auto messy = parse_parametric_eq("\r\n  # comment\r\n  Preamp: -1.5 dB  \r\n"
                                     "  Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1  \r\n\r\n");
    ok &= check(messy.has_value() && near(messy->preamp_db, -1.5, 1e-9) && messy->bands.size() == 1,
                "parse: CRLF/注释/空白容错");

    // 省略 Gain 的 LP 行(按关键字扫描而非固定位置的回归守卫)
    auto lp = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON LP Fc 8000 Hz Q 0.71\n");
    ok &= check(lp.has_value() && lp->bands.size() == 1 && lp->bands[0].type == HptfBandType::low_pass,
                "parse: LP 行(无 Gain)");
    ok &= check(lp.has_value() && near(lp->bands[0].gain_db, 0.0, 1e-12), "parse: 缺 Gain 按 0 dB");

    // 未知类型跳过而非致命
    auto unknown = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON ZZZ Fc 1000 Hz Gain 3 dB Q 1\n"
                                       "Filter 2: ON PK Fc 2000 Hz Gain 3 dB Q 1\n");
    ok &= check(unknown.has_value() && unknown->bands.size() == 1, "parse: 未知类型跳过");

    // 拒绝畸形输入
    ok &= check(!parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 3 dB Q 0\n").has_value(),
                "parse: Q=0 被拒绝");
    ok &= check(!parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc abc Hz Gain 3 dB Q 1\n").has_value(),
                "parse: Fc 非数值被拒绝");
    ok &= check(!parse_parametric_eq("").has_value(), "parse: 空内容被拒绝");

    // 小数点必须按 C locale 解析(非 C locale 下 "105.5" 会变成 105)
    auto frac = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 105.5 Hz Gain 3 dB Q 1\n");
    ok &= check(frac.has_value() && near(frac->bands[0].fc_hz, 105.5, 1e-9), "parse: 小数点按 C locale");

    return ok;
}

// ── (b) 频响 ──────────────────────────────────────────────────────────────────

bool test_response() {
    bool ok = true;
    const auto coeffs = design_mv1();
    ok &= check(coeffs.band_count == 10, "design: 10 段进级联");

    // 强形式:**运行中的滤波器**与**解析公式**一致。解析公式正是驱动 auto_trim 的那份。
    for (const double hz : {50.0, 100.0, 250.0, 700.0, 1700.0, 3000.0, 5800.0, 12000.0}) {
        const double measured = impulse_response_db(coeffs, hz);
        const double analytic = cascade_magnitude_db(coeffs, hz);
        ok &= check(near(measured, analytic, 0.05),
                    "response: 冲激响应与解析式一致 @" + std::to_string(static_cast<int>(hz)) + "Hz");
    }

    // 绝对刻度锚定在外部核对过的 ground truth(含 -4.1 dB preamp)
    ok &= check(near(cascade_magnitude_db(coeffs, 100.0), -5.04, 0.1), "response: 100 Hz ≈ -5.04 dB");
    ok &= check(near(cascade_magnitude_db(coeffs, 1700.0), -1.36, 0.1), "response: 1700 Hz ≈ -1.36 dB");
    ok &= check(near(cascade_magnitude_db(coeffs, 5800.0), -8.47, 0.1), "response: 5800 Hz ≈ -8.47 dB");
    ok &= check(near(static_cast<double>(coeffs.max_response_db), -0.10, 0.1), "response: 峰值 ≈ -0.10 dB");

    // 单段 PK:中心处恰好等于 gain,远离中心趋于 0
    auto one = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 2\n");
    auto oc = design_cascade(*one, k_fs, HptfPreampMode::warn_only);
    ok &= check(near(cascade_magnitude_db(*oc, 1000.0), 6.0, 0.01), "response: 单段 PK 中心 = +6 dB");
    ok &= check(near(cascade_magnitude_db(*oc, 1000.0 / 64.0), 0.0, 0.05), "response: 单段 PK 远低频 ≈ 0");

    // 采样率无关性:同一 profile 在不同 fs 下,中心频率处响应一致
    auto c44 = design_cascade(*one, 44100, HptfPreampMode::warn_only);
    auto c96 = design_cascade(*one, 96000, HptfPreampMode::warn_only);
    ok &= check(near(cascade_magnitude_db(*c44, 1000.0), 6.0, 0.02), "response: 44.1k 中心 = +6 dB");
    ok &= check(near(cascade_magnitude_db(*c96, 1000.0), 6.0, 0.02), "response: 96k 中心 = +6 dB");

    // fc 越界的段在设计期跳过(20 kHz 段在 48k 合法、在 32k 不合法)
    auto high = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 20000 Hz Gain 3 dB Q 1\n");
    auto h48 = design_cascade(*high, 48000, HptfPreampMode::warn_only);
    auto h32 = design_cascade(*high, 32000, HptfPreampMode::warn_only);
    ok &= check(h48.has_value() && h48->band_count == 1, "design: 20 kHz 段在 48k 保留");
    ok &= check(h32.has_value() && h32->band_count == 0, "design: 20 kHz 段在 32k 跳过");

    return ok;
}

// ── (c) Preamp 开关 ───────────────────────────────────────────────────────────

bool test_preamp_mode() {
    bool ok = true;
    // 峰值 +6 dB 且没有 preamp 的曲线
    auto hot = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 2\n");

    auto warn = design_cascade(*hot, k_fs, HptfPreampMode::warn_only);
    ok &= check(warn.has_value() && near(static_cast<double>(warn->max_response_db), 6.0, 0.05),
                "preamp: warn_only 报出 +6 dB 峰值");
    ok &= check(warn.has_value() && warn->auto_trim_db == 0.0F, "preamp: warn_only 不修改增益");

    auto trim = design_cascade(*hot, k_fs, HptfPreampMode::auto_trim);
    ok &= check(trim.has_value() && trim->auto_trim_db < -5.9F, "preamp: auto_trim 压掉约 6 dB");
    // 重新测量已设计好的级联:峰值必须 <= 0
    double peak = -1000.0;
    for (int i = 0; i < 512; ++i) {
        const double hz = 20.0 * std::exp((std::log(20000.0 / 20.0) / 511.0) * i);
        peak = std::max(peak, cascade_magnitude_db(*trim, hz));
    }
    ok &= check(peak <= 0.001, "preamp: auto_trim 后实测峰值 <= 0 dB");

    // MDR-MV1 本来就在安全档,auto_trim 不该改动它
    const auto mv1_trim = design_mv1(HptfPreampMode::auto_trim);
    ok &= check(mv1_trim.auto_trim_db == 0.0F, "preamp: 已安全的曲线 auto_trim 不动");

    return ok;
}

// ── (d) bypass 必须逐样本 bit-identical ──────────────────────────────────────

std::vector<float> lcg_noise(std::size_t frames, std::uint32_t channels, std::uint32_t seed) {
    std::vector<float> out(frames * channels);
    std::uint32_t s = seed;
    for (auto& v : out) {
        s = (s * 1664525U) + 1013904223U;
        v = (static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U)) - 0.5F;
    }
    return out;
}

bool test_bypass_bit_identical() {
    bool ok = true;
    for (const std::size_t frames : {std::size_t{1}, std::size_t{64}, std::size_t{1024}, std::size_t{4096}}) {
        const auto reference = lcg_noise(frames, 2, 12345U);
        auto buf = reference;

        HptfProcessor proc;
        proc.prepare(2, k_fs);
        proc.process(buf.data(), frames);

        bool same = true;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] != reference[i]) {
                same = false;
                break;
            }
        }
        ok &= check(same, "bypass: 未发布任何 profile 时逐样本 bit-identical (frames=" + std::to_string(frames) + ")");
    }

    // 发布 → 混合完成 → 再发布 bypass → 混合完成,之后必须重新回到精确短路,
    // 证明恒等路径是真短路而不是 x*1.0f + y*0.0f。
    {
        HptfProcessor proc;
        proc.prepare(2, k_fs);
        proc.publish(design_mv1(), 1);
        std::vector<float> warm(k_hptf_blend_frames * 2U * 2U, 0.1F);
        proc.process(warm.data(), k_hptf_blend_frames * 2U);
        proc.publish_bypass(2);
        proc.process(warm.data(), k_hptf_blend_frames * 2U);
        ok &= check(proc.applied_revision() == 2, "bypass: revision 前进到 2");

        const std::size_t frames = 512;
        const auto reference = lcg_noise(frames, 2, 999U);
        auto buf = reference;
        proc.process(buf.data(), frames);
        bool same = true;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] != reference[i]) {
                same = false;
                break;
            }
        }
        ok &= check(same, "bypass: 关闭后重新回到精确短路");
    }
    return ok;
}

// ── (e) 热切换 ────────────────────────────────────────────────────────────────

bool test_hot_swap() {
    bool ok = true;
    constexpr std::size_t k_block = 256;
    constexpr double k_sine_hz = 1000.0;

    const auto a = design_mv1();
    auto b_profile = parse_parametric_eq("Preamp: -6 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1.5\n");
    const auto b = *design_cascade(*b_profile, k_fs, HptfPreampMode::warn_only);

    HptfProcessor proc;
    proc.prepare(1, k_fs);
    proc.publish(a, 1);

    // 先让 A 完全稳定
    std::size_t n = 0;
    std::vector<float> buf(k_block);
    auto fill_sine = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const double t = static_cast<double>(n + i) / static_cast<double>(k_fs);
            buf[i] = static_cast<float>(0.5 * std::sin(2.0 * std::acos(-1.0) * k_sine_hz * t));
        }
    };
    for (int i = 0; i < 40; ++i) {
        fill_sine(k_block);
        proc.process(buf.data(), k_block);
        n += k_block;
    }

    // 切到 B,记录整个混合期间的输出
    proc.publish(b, 2);
    std::vector<float> captured;
    captured.reserve(k_hptf_blend_frames * 3U);
    for (std::size_t i = 0; i < (k_hptf_blend_frames * 3U) / k_block; ++i) {
        fill_sine(k_block);
        proc.process(buf.data(), k_block);
        captured.insert(captured.end(), buf.begin(), buf.end());
        n += k_block;
    }

    // 1 kHz / 48 kHz 正弦的解析最大一阶差分;留 2% 余量。硬切会立刻破这条线。
    const double w = 2.0 * std::acos(-1.0) * k_sine_hz / static_cast<double>(k_fs);
    const double max_step = 0.5 * 2.0 * std::sin(w / 2.0) * 1.3; // 幅度上限约 1.3(A/B 在 1 kHz 附近有增益)
    double worst_d1 = 0.0;
    double worst_d2 = 0.0;
    for (std::size_t i = 2; i < captured.size(); ++i) {
        worst_d1 = std::max(worst_d1, std::fabs(static_cast<double>(captured[i] - captured[i - 1])));
        const double d2 = static_cast<double>(captured[i]) - (2.0 * captured[i - 1]) + captured[i - 2];
        worst_d2 = std::max(worst_d2, std::fabs(d2));
    }
    ok &= check(worst_d1 <= max_step * 1.02, "hot-swap: 一阶差分无跳变");
    ok &= check(worst_d2 <= max_step * 0.2, "hot-swap: 二阶差分无折点");

    ok &= check(proc.applied_revision() == 2, "hot-swap: 混合结束后 revision = 2");

    // 混合结束后,输出应与"B 从一开始就在跑"一致
    HptfProcessor pure;
    pure.prepare(1, k_fs);
    pure.publish(b, 1);
    std::vector<float> pure_buf(k_block);
    std::size_t m = 0;
    for (int i = 0; i < 80; ++i) {
        for (std::size_t j = 0; j < k_block; ++j) {
            const double t = static_cast<double>(m + j) / static_cast<double>(k_fs);
            pure_buf[j] = static_cast<float>(0.5 * std::sin(2.0 * std::acos(-1.0) * k_sine_hz * t));
        }
        pure.process(pure_buf.data(), k_block);
        m += k_block;
    }
    // 对齐到同一相位:两边都取各自最后一块里的峰值幅度
    auto peak_of = [](const std::vector<float>& v, std::size_t from) {
        double p = 0.0;
        for (std::size_t i = from; i < v.size(); ++i) {
            p = std::max(p, std::fabs(static_cast<double>(v[i])));
        }
        return p;
    };
    const double after = peak_of(captured, captured.size() - k_block);
    const double expected = peak_of(pure_buf, 0);
    ok &= check(near(after, expected, 1e-3), "hot-swap: 混合后收敛到 B 的稳态幅度");

    // 混合确实发生过(中途存在介于两者之间的样本),而不是瞬间硬切
    const double a_gain = std::pow(10.0, cascade_magnitude_db(a, k_sine_hz) / 20.0) * 0.5;
    const double b_gain = std::pow(10.0, cascade_magnitude_db(b, k_sine_hz) / 20.0) * 0.5;
    const double lo = std::min(a_gain, b_gain);
    const double hi = std::max(a_gain, b_gain);
    // 混合窗中点附近的幅度应严格落在 A、B 幅度之间——证明确实混合过,而非瞬间硬切
    bool saw_between = false;
    const std::size_t mid = static_cast<std::size_t>(k_hptf_blend_frames / 2);
    if (mid + k_block < captured.size()) {
        double mid_peak = 0.0;
        for (std::size_t i = mid; i < mid + k_block; ++i) {
            mid_peak = std::max(mid_peak, std::fabs(static_cast<double>(captured[i])));
        }
        saw_between = (mid_peak > lo * 0.98) && (mid_peak < hi * 1.02) && (hi - lo) > 1e-4;
    }
    ok &= check(saw_between, "hot-swap: 混合中点幅度落在 A/B 之间");

    return ok;
}

// ── 数值稳健性 ────────────────────────────────────────────────────────────────

bool test_numeric_robustness() {
    bool ok = true;
    // 20 Hz 高 Q 段:float32 状态在此区域会丢精度。长时间跑必须不发散。
    auto low = parse_parametric_eq("Preamp: 0 dB\nFilter 1: ON PK Fc 20 Hz Gain 10 dB Q 8\n");
    auto coeffs = design_cascade(*low, k_fs, HptfPreampMode::warn_only);

    HptfCascade cascade;
    cascade.prepare(2);
    cascade.set_coefficients(*coeffs);

    auto noise = lcg_noise(4096, 2, 4242U);
    double worst = 0.0;
    for (int block = 0; block < 200; ++block) {
        auto buf = noise;
        cascade.process(buf.data(), 4096);
        for (const float v : buf) {
            if (!std::isfinite(v)) {
                ok &= check(false, "numeric: 输出出现非有限值");
                return ok;
            }
            worst = std::max(worst, std::fabs(static_cast<double>(v)));
        }
    }
    ok &= check(worst < 100.0, "numeric: 20 Hz Q=8 长跑不发散");

    // frames == 0 不得触碰任何状态
    std::vector<float> empty;
    cascade.process(empty.data(), 0);
    ok &= check(true, "numeric: frames==0 安全返回");

    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_parse();
    ok &= test_response();
    ok &= test_preamp_mode();
    ok &= test_bypass_bit_identical();
    ok &= test_hot_swap();
    ok &= test_numeric_robustness();

    if (ok) {
        std::cout << "hptf eq tests passed\n";
        return 0;
    }
    return 1;
}
