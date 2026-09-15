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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/options.h" // HptfPreampMode(公共选项枚举,GUI / C ABI 共用)

namespace mradm::render_common {

// 级联最多容纳的滤波器段数。AutoEq 的 ParametricEQ 常见 5~10 段,32 段留足余量,
// 同时让 HptfCoefficients 保持定长 POD(可整体 memcpy 发布给音频回调)。
inline constexpr std::size_t k_hptf_max_bands = 32;

// 交叉淡化长度。下界不是爆音感知(任何连续混合都不爆音),而是**被淡出频段自身的
// 衰减时间**:46 Hz 低 Q 段与 105 Hz 搁架要振铃数十毫秒,窗口短于此会听到频谱抖动
// 而非平滑过渡。上界是 A/B 的"即时感"。取值与 MonitorEngine 的流交叉淡化常量一致,
// 使"同时换后端 + 换 profile"的行为可预期。
inline constexpr std::uint64_t k_hptf_blend_frames = 2048;

// 回调内分块处理的粒度;宿主给多大的块都不需要再分配。
inline constexpr std::size_t k_hptf_chunk_frames = 1024;

enum class HptfBandType : std::uint8_t {
    peaking,
    low_shelf,
    high_shelf,
    low_pass,
    high_pass,
    band_pass,
    notch,
};

// 一条解析出来的滤波器行。**与采样率无关**——重建引擎时按新采样率重跑
// design_cascade() 即可,不必重新读文件。
struct HptfBand {
    HptfBandType type{HptfBandType::peaking};
    bool enabled{true};
    double fc_hz{1000.0};
    double gain_db{0.0};
    double q{0.707};
};

// 一份解析结果(= 一个 ParametricEQ.txt)。
struct HptfProfile {
    double preamp_db{0.0};
    std::vector<HptfBand> bands;
    std::string name; // 展示用;通常是文件名
};

// 归一化后的双二阶段(a0 已除掉)。
struct HptfBiquad {
    float b0{1.0F};
    float b1{0.0F};
    float b2{0.0F};
    float a1{0.0F};
    float a2{0.0F};
};

// 设计结果。**定长 POD**:不含 vector/string,所以可以整体字节拷贝发布给音频回调,
// 这正是无锁交接得以成立的前提。band_count == 0 表示 bypass。
struct HptfCoefficients {
    std::uint32_t sample_rate{0};
    std::uint32_t band_count{0};
    float preamp_gain{1.0F};
    std::array<HptfBiquad, k_hptf_max_bands> sections{};
    float max_response_db{0.0F}; // 含 preamp;> 0 表示这条曲线可能削波
    float auto_trim_db{0.0F};    // warn_only 下恒为 0
};

// ── 解析 ──────────────────────────────────────────────────────────────────────

// 纯文本 → profile,不碰文件系统,便于直接单测。
//
// 容错:跳过空行与 '#' 注释;`Filter N: OFF ...` 会保留但标记 enabled=false;
// 未知滤波器类型跳过(AutoEq 的类型词表会漂移)。数值解析**强制 C locale**——
// 非 C locale 下 "105.5" 会被解析成 105。
[[nodiscard]] Result<HptfProfile> parse_parametric_eq(std::string_view text);

// 读文件后调用上面那个;错误的 context 带上路径(ADR 0005)。
[[nodiscard]] Result<HptfProfile> load_parametric_eq_file(const std::filesystem::path& path);

// ── 设计 ──────────────────────────────────────────────────────────────────────

// RBJ Audio-EQ-Cookbook。搁架用 **Q 参数化**形式(不是 slope S 形式)——这才是
// EqualizerAPO / AutoEq 的 LSC/HSC 含义。fc <= 0 或 fc >= fs/2 的段在**设计期**跳过
// (20 kHz 段在 48 kHz 合法、在 32 kHz 不合法)。
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
    void prepare(std::uint32_t channels);

    // 纯字节拷贝,回调安全。
    void set_coefficients(const HptfCoefficients& coeffs) noexcept;

    void reset() noexcept;

    // 就地处理 interleaved PCM。严格无分配、无锁、无 I/O。
    // bypass(band_count == 0)时**精确短路**——不做 x * 1.0f,保证逐样本 bit-identical。
    void process(float* interleaved, std::size_t frames) noexcept;

    [[nodiscard]] const HptfCoefficients& coefficients() const noexcept { return coeffs_; }
    [[nodiscard]] bool is_bypass() const noexcept { return coeffs_.band_count == 0; }
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
// 无法迁移。让两条级联吃同一份输入,各自对真实输入始终处于稳态,于是混合结束时
// 切进来的那条的状态恰好等于"它一直在跑"应有的状态——尾端无瞬态,而起点它的权重是 0。
//
// 为什么是**线性**混合而不是等功率:两条级联吃同一输入、输出强相关,等功率律会在
// 中点鼓出约 +3 dB。这与"两路不相关信号交叉淡化"的常规相反。
class HptfProcessor {
  public:
    // 控制线程,构造期调用一次。channels 是**输出**宽度。
    void prepare(std::uint32_t channels, std::uint32_t sample_rate);

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    // 控制线程:发布一组新系数。写非活动槽后以 release 发布;在此之前会自旋等待回调
    // 退出拷贝窗口(写者可以阻塞,回调不可以)。
    void publish(const HptfCoefficients& coeffs, std::uint64_t revision);

    // 控制线程:发布 bypass(band_count == 0)。启用 / 关闭 / 换 profile 因此是同一条
    // 代码路径、同一次混合,没有"关掉时"的特例爆音。
    void publish_bypass(std::uint64_t revision);

    // **仅在回调已停泊时调用**(monitor 的 apply_seek_locked / Scene 的 begin_epoch)。
    // 清滤波器状态并结束在飞的混合。
    void reset_state() noexcept;

    // 回调已应用的 revision,供状态轮询确认编辑落地。
    [[nodiscard]] std::uint64_t applied_revision() const noexcept;

    // 当前生效的系数快照(控制线程读,用于 *_get_hptf_info)。
    [[nodiscard]] HptfCoefficients active_coefficients() const noexcept;

    // 音频回调:就地处理 interleaved PCM。无分配、无锁、无 I/O。
    void process(float* interleaved, std::size_t frames) noexcept;

  private:
    void finish_blend() noexcept;

    // 写者拥有的双槽。回调只读、从不写槽。
    std::array<HptfCoefficients, 2> slots_{};
    std::array<std::uint64_t, 2> slot_revisions_{};

    std::atomic<std::uint32_t> pending_{0}; // 代号;槽下标 = pending_ & 1
    std::atomic<std::uint32_t> in_copy_{0}; // 回调正在拷贝某个槽
    std::atomic<std::uint64_t> applied_revision_{0};

    std::uint32_t generation_{0}; // 写者私有

    // 回调私有状态。
    HptfCascade front_;
    HptfCascade back_;
    std::uint32_t consumed_generation_{0};
    bool blending_{false};
    std::uint64_t blend_pos_{0};
    std::uint64_t blend_revision_{0};
    std::vector<float> scratch_;

    std::uint32_t channels_{0};
    std::uint32_t sample_rate_{0};
};

} // namespace mradm::render_common
