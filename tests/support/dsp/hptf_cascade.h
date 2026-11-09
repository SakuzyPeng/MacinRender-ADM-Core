#pragma once

// Test-only HpTF helpers (formerly in src/adm_render_common/hptf_eq.*): the closed-form design
// and response, and one stand-alone cascade, over the private Rust DSP boundary. Production uses
// the hot-swappable processor (HptfProcessor / the Scene output's Rust session).
#include <cstddef>
#include <cstdint>
#include <memory>

#include "hptf_eq.h"

namespace mradm::render_common {

// RBJ Audio-EQ-Cookbook。搁架用 **Q 参数化**形式(不是 slope S 形式)——这才是
// EqualizerAPO / AutoEq 的 LSC/HSC 含义。非法参数/不稳定系数返回错误;fc >= fs/2
// 的段在设计期跳过(20 kHz 段在 48 kHz 合法、在 32 kHz 不合法)。
[[nodiscard]] Result<HptfCoefficients>
design_cascade(const HptfProfile& profile, std::uint32_t sample_rate, HptfPreampMode mode);

// 级联在 hz 处的幅度响应(dB,含 preamp)。auto_trim 的搜索与频响测试都用这个闭式,
// 所以测试断言的正是驱动 auto_trim 的同一份公式。
[[nodiscard]] double cascade_magnitude_db(const HptfCoefficients& coeffs, double hz);

// 一条多声道级联 + 其状态。转置直接 II 型(TDF2),**float 系数 + double 状态**:
// AutoEq profile 常含 20~46 Hz 的段,48 kHz 下极点半径逼近 1,float32 状态在该区域
// 会丢精度并可能在大正增益下累积直流;10 段 × 2 声道的 double 状态只有 320 字节。
class HptfCascade {
  public:
    HptfCascade();
    ~HptfCascade();
    HptfCascade(const HptfCascade& other);
    HptfCascade& operator=(const HptfCascade& other);
    HptfCascade(HptfCascade&& other) noexcept;
    HptfCascade& operator=(HptfCascade&& other) noexcept;
    // 控制线程:一次性分配状态。之后 set_coefficients / process 都不再分配。
    void prepare(std::uint32_t channel_count);

    // 复制并检查已设计系数,不分配、不清滤波历史,回调安全。
    void set_coefficients(const HptfCoefficients& coeffs) noexcept;

    void reset() noexcept;

    // 就地处理 interleaved PCM。严格无分配、无锁、无 I/O。
    // 零段且单位增益时精确短路;零段的非单位前级仍生效。
    void process(float* interleaved, std::size_t frames) noexcept;

    [[nodiscard]] const HptfCoefficients& coefficients() const noexcept { return coeffs_; }
    [[nodiscard]] bool is_bypass() const noexcept { return coeffs_.is_bypass(); }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

  private:
    HptfCoefficients coeffs_{};
    std::uint32_t channels_{0};
    struct State;
    std::unique_ptr<State> state_; // Rust owns the double state; copies clone history.
};

} // namespace mradm::render_common
