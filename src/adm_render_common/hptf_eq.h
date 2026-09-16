#pragma once

// HpTF(耳机传输函数)补偿:把 AutoEq 的 ParametricEQ 文本转成 biquad 级联,施加在
// **设备绑定的立体声耳机馈送**上。
//
// 边界约定(与 StereoPeakGuard 同级):HpTF 只作用于最终交给耳机的 2 声道 PCM。
// 多声道扬声器馈送、交给 OS 做 HRTF 的 system-spatial 床、以及调用方自取 PCM 的
// 裸 pull 路径都**不施加**——那些路径上"最终耳机信号"要么不存在,要么不归我们生成。
//
// 本模块只依赖标准库(ADR 0003:mr_adm_render_common 零第三方依赖)。
//
// 位精确性:HpTF 只走实时监听、永不进入离线母版,因此**不要求跨平台位精确**,
// 一致性工具无需为它建立检查点(参见 docs/architecture/CONSISTENCY_LOCALIZATION.md)。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/hptf.h"
#include "adm/options.h" // HptfPreampMode(公共选项枚举,GUI / C ABI 共用)

namespace mradm::render_common {

// 级联最多容纳的滤波器段数。AutoEq 的 ParametricEQ 常见 5~10 段,32 段留足余量,
// 同时让 HptfCoefficients 保持定长 POD(可整体 memcpy 发布给音频回调)。
using mradm::k_hptf_max_bands;

// 交叉淡化长度。下界不是爆音感知(任何连续混合都不爆音),而是**被淡出频段自身的
// 衰减时间**:46 Hz 低 Q 段与 105 Hz 搁架要振铃数十毫秒,窗口短于此会听到频谱抖动
// 而非平滑过渡。上界是 A/B 的"即时感"。取值与 MonitorEngine 的流交叉淡化常量一致,
// 使"同时换后端 + 换 profile"的行为可预期。
inline constexpr std::uint64_t k_hptf_blend_frames = 2048;

// 回调内分块处理的粒度;宿主给多大的块都不需要再分配。
inline constexpr std::size_t k_hptf_chunk_frames = 1024;

using mradm::HptfBand;
using mradm::HptfBandType;
using mradm::HptfProfile;

// 归一化后的双二阶段(a0 已除掉)。
struct HptfBiquad {
    float b0{1.0F};
    float b1{0.0F};
    float b2{0.0F};
    float a1{0.0F};
    float a2{0.0F};
};

// 设计结果。**定长 POD**:不含 vector/string,所以可以整体字节拷贝发布给音频回调,
// 这正是无锁交接得以成立的前提。零段仍可只施加前级增益。
struct HptfCoefficients {
    std::uint32_t sample_rate{0};
    std::uint32_t band_count{0};
    float preamp_gain{1.0F};
    std::array<HptfBiquad, k_hptf_max_bands> sections{};
    float max_response_db{0.0F}; // 含 preamp;> 0 表示这条曲线可能削波
    float auto_trim_db{0.0F};    // warn_only 下恒为 0
    float preamp_db{0.0F};       // 文件里的前级值,随同一 revision 发布

    [[nodiscard]] bool is_bypass() const noexcept { return band_count == 0 && preamp_gain == 1.0F; }
};

struct HptfSnapshot {
    HptfCoefficients coefficients;
    std::uint64_t revision{0};
};

// Single producer / single consumer, with private read/write slots and one exchanged slot.
// Neither side touches a slot owned by the other. A newer publication replaces pending data;
// every consume is bounded and never waits. External control callers serialize their own side.
class HptfMailbox {
  public:
    void publish(const HptfSnapshot& snapshot) noexcept;
    [[nodiscard]] std::optional<HptfSnapshot> consume() noexcept;

  private:
    static constexpr std::uint32_t k_dirty = 4U;
    static constexpr std::uint32_t k_index_mask = 3U;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::array<HptfSnapshot, 3> slots_{};
    std::atomic<std::uint32_t> middle_{2U};
    std::uint32_t write_slot_{0U};
    std::uint32_t read_slot_{1U};
};

// ── 解析 ──────────────────────────────────────────────────────────────────────

// 纯文本 → profile,不碰文件系统,便于直接单测。
//
// 容错:跳过空行与 '#' 注释;`Filter N: OFF ...` 会保留但标记 enabled=false;
// 未知滤波器类型跳过(AutoEq 的类型词表会漂移)。数值解析**强制 C locale**——
// 非 C locale 下 "105.5" 会被解析成 105。
[[nodiscard]] Result<HptfProfile> parse_parametric_eq(std::string_view text);

// Shared validation for imported and structured profiles, including disabled bands.
[[nodiscard]] Result<void> validate_hptf_profile(const HptfProfile& profile);

// 读文件后调用上面那个;错误的 context 带上路径(ADR 0005)。
[[nodiscard]] Result<HptfProfile> load_parametric_eq_file(const std::filesystem::path& path);

// ── 设计 ──────────────────────────────────────────────────────────────────────

// RBJ Audio-EQ-Cookbook。搁架用 **Q 参数化**形式(不是 slope S 形式)——这才是
// EqualizerAPO / AutoEq 的 LSC/HSC 含义。非法参数/不稳定系数返回错误;fc >= fs/2
// 的段在设计期跳过(20 kHz 段在 48 kHz 合法、在 32 kHz 不合法)。
[[nodiscard]] Result<HptfCoefficients>
design_cascade(const HptfProfile& profile, std::uint32_t sample_rate, HptfPreampMode mode);

// 级联在 hz 处的幅度响应(dB,含 preamp)。auto_trim 的搜索与频响测试都用这个闭式,
// 所以测试断言的正是驱动 auto_trim 的同一份公式。
[[nodiscard]] double cascade_magnitude_db(const HptfCoefficients& coeffs, double hz);

// ── 运行 ──────────────────────────────────────────────────────────────────────

// 一条多声道级联 + 其状态。转置直接 II 型(TDF2),**float 系数 + double 状态**:
// AutoEq profile 常含 20~46 Hz 的段,48 kHz 下极点半径逼近 1,float32 状态在该区域
// 会丢精度并可能在大正增益下累积直流;10 段 × 2 声道的 double 状态只有 320 字节。
class HptfCascade {
  public:
    // 控制线程:一次性分配状态。之后 set_coefficients / process 都不再分配。
    void prepare(std::uint32_t channel_count);

    // 纯字节拷贝,回调安全。
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
    std::vector<double> state_; // [channels × k_hptf_max_bands × 2]
};

// 可热切换的 HpTF 处理器:控制线程发布系数,音频回调消费,切换时两条级联并行跑一段
// 再线性混合。
//
// 为什么是"双级联并行 + 混合"而不是插值系数:两份 profile 的段数可以不同,而且在
// a1/a2 之间插值会经过不稳定区;更根本的是,不同传递函数之间**状态没有对应关系**,
// 无法直接迁移。两条级联处理同一输入,新级联从零状态开始并逐渐淡入。
// 高 Q 滤波器的收敛可能长于混合窗,因此不声称有限淡入能恢复无限长输入历史。
//
// 为什么是**线性**混合而不是等功率:两条级联吃同一输入、输出强相关,等功率律会在
// 中点鼓出约 +3 dB。这与"两路不相关信号交叉淡化"的常规相反。
class HptfProcessor {
  public:
    // 控制线程,构造期调用一次。channels 是**输出**宽度。
    void prepare(std::uint32_t channel_count, std::uint32_t rate);

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    // 控制线程:写私有槽后交换所有权。互斥锁只串行化控制调用,回调从不获取它。
    void publish(const HptfCoefficients& coeffs, std::uint64_t revision);

    // 控制线程:发布单位增益的零段 bypass。启用 / 关闭 / 换 profile 因此是同一条
    // 代码路径、同一次混合,没有"关掉时"的特例爆音。
    void publish_bypass(std::uint64_t revision);

    // **仅在回调已停泊时调用**(monitor 的 apply_seek_locked / Scene 的 begin_epoch)。
    // 采用最新已发布目标(或当前混合目标),再清历史;seek 不会丢失配置。
    void reset_state() noexcept;

    // 回调已应用的 revision,供状态轮询确认编辑落地。
    [[nodiscard]] std::uint64_t applied_revision() const;

    // 当前生效的系数快照(控制线程读,用于 *_get_hptf_info)。
    [[nodiscard]] HptfCoefficients active_coefficients() const;
    [[nodiscard]] HptfSnapshot active_snapshot() const;

    // 音频回调:就地处理 interleaved PCM。无分配、无锁、无 I/O。
    void process(float* interleaved, std::size_t frames) noexcept;

  private:
    void finish_blend() noexcept;

    std::mutex publication_mutex_; // control producers only
    HptfMailbox pending_;
    mutable std::mutex status_mutex_; // control readers only
    mutable HptfMailbox status_;
    mutable HptfSnapshot observed_;

    // 回调私有状态。
    HptfCascade front_;
    HptfCascade back_;
    std::uint64_t front_revision_{0};
    bool blending_{false};
    std::uint64_t blend_pos_{0};
    std::uint64_t blend_revision_{0};
    std::vector<float> scratch_;

    std::uint32_t channels_{0};
    std::uint32_t sample_rate_{0};
};

} // namespace mradm::render_common
