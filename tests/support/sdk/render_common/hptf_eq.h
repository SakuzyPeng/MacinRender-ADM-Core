#pragma once

// HpTF(耳机传输函数)补偿:把 AutoEq 的 ParametricEQ 文本转成 biquad 级联,施加在
// **设备绑定的立体声耳机馈送**上。
//
// 边界约定(与 StereoPeakGuard 同级):HpTF 只作用于最终交给耳机的 2 声道 PCM。
// 多声道扬声器馈送、交给 OS 做 HRTF 的 system-spatial 床、以及调用方自取 PCM 的
// 裸 pull 路径都**不施加**——那些路径上"最终耳机信号"要么不存在,要么不归我们生成。
//
// C++ 只转换参数记录；设计、发布交接与处理都在私有 Rust DSP 边界（mradm-dsp `hptf::Live`）。
//
// 数值与检查点遵循 ADR 0018；旧共享路径维持既有逐位结果。

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/hptf.h"
#include "adm/monitor.h"
#include "adm/options.h" // HptfPreampMode(公共选项枚举,GUI / C ABI 共用)

#include "../../ffi/dsp_test_ffi.h"

// The private Rust records (dsp_ffi.h), for callers that hand coefficients to another Rust owner.
struct MradmDspHptfCoefficients;
struct MradmDspHptfSnapshot;

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
    bool operator==(const HptfBiquad&) const = default;
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

    bool operator==(const HptfCoefficients&) const = default;
    [[nodiscard]] bool is_bypass() const noexcept { return band_count == 0 && preamp_gain == 1.0F; }
};

struct HptfSnapshot {
    HptfCoefficients coefficients;
    std::uint64_t revision{0};
    HptfCoefficients right{};
    bool independent{false};
};

// ── 解析 ──────────────────────────────────────────────────────────────────────

// 纯文本 → profile,不碰文件系统,便于直接单测。
//
// 容错:跳过空行与 '#' 注释;`Filter N: OFF ...` 会保留但标记 enabled=false;
// 未知滤波器类型跳过(AutoEq 的类型词表会漂移)。数值解析**强制 C locale**——
// 非 C locale 下 "105.5" 会被解析成 105。
[[nodiscard]] Result<HptfProfile> parse_parametric_eq(std::string_view text);

// Rust owns both parameter copies, including the legacy shared configuration.
struct HptfSettings {
    std::shared_ptr<void> handle;
    [[nodiscard]] Result<HptfSnapshot> design(std::uint32_t rate) const;
};
[[nodiscard]] Result<HptfSettings> prepare_hptf(const void* config, std::uint32_t source_kind);
[[nodiscard]] Result<HptfSettings>
prepare_hptf(const HptfProfile& profile, HptfPreampMode mode, std::uint64_t revision);
[[nodiscard]] Result<HptfSettings> prepare_hptf(const std::string& path, HptfPreampMode mode, std::uint64_t revision);
[[nodiscard]] HptfStereoInfo hptf_snapshot_info(const HptfSnapshot& snapshot);
[[nodiscard]] MradmDspHptfCoefficients hptf_record(const HptfCoefficients& coefficients) noexcept;
[[nodiscard]] HptfSnapshot hptf_snapshot(const MradmDspHptfSnapshot& record) noexcept;

// ── 运行 ──────────────────────────────────────────────────────────────────────

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
    HptfProcessor();
    ~HptfProcessor();
    HptfProcessor(const HptfProcessor&) = delete;
    HptfProcessor& operator=(const HptfProcessor&) = delete;
    HptfProcessor(HptfProcessor&&) = delete;
    HptfProcessor& operator=(HptfProcessor&&) = delete;
    // 控制线程,构造期调用一次。channels 是**输出**宽度。
    void prepare(std::uint32_t channel_count, std::uint32_t rate);
    // The processor of a Rust monitor (mradm_engine_live_monitor_hptf), which outlives this view.
    void borrow(const void* live, std::uint32_t channel_count, std::uint32_t rate);

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    // 控制线程:目标交给 Rust 的三槽交接,回调从不等待。处理器不接受的目标在发布时抛出。
    void publish(const HptfCoefficients& coeffs, std::uint64_t revision);
    void publish(const HptfSnapshot& snapshot);

    // 控制线程:发布单位增益的零段 bypass。启用 / 关闭 / 换 profile 因此是同一条
    // 代码路径、同一次混合,没有"关掉时"的特例爆音。
    void publish_bypass(std::uint64_t revision);

    // **仅在回调已停泊时调用**(monitor 的 apply_seek_locked)。
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
    struct State;
    std::unique_ptr<State> state_;
    std::uint32_t channels_{0};
    std::uint32_t sample_rate_{0};
};

} // namespace mradm::render_common
