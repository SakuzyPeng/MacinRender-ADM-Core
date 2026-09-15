#include "hptf_eq.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <fstream>
#include <ios>
#include <locale>
#include <sstream>
#include <thread>
#include <utility>

namespace mradm::render_common {
namespace {

constexpr double k_default_q = 0.707;

// auto_trim / max_response 的扫描网格:**可听带 20 Hz – 20 kHz**(上限再夹到 Nyquist 以下),
// 对数等分。
//
// 刻意不往 20 Hz 以下扫。以 MDR-MV1 为例,+9.7 dB 的 105 Hz 低架在 46 Hz 凹陷影响消退后
// 会一路抬起来:20 Hz 处 -0.52 dB,10 Hz 处 +2.93 dB,5 Hz 处 +4.75 dB。若把次声区计入峰值,
// auto_trim 会为了保护无耳机能重放、也几乎不存在于节目里的频率,把整条曲线再压掉近 5 dB。
// AutoEq 自己的 Preamp 也是按可听带算的,取同一约定才能与它的标称值对得上。
constexpr int k_scan_points = 512;
constexpr double k_scan_low_hz = 20.0;
constexpr double k_scan_high_hz = 20000.0;

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// 强制 C locale 的浮点解析。**不要**换成 std::stod 或默认 locale 的 istringstream:
// 非 C locale 下 "105.5" 会被解析成 105,而 Windows CI 跑在 runner 的 locale 下。
[[nodiscard]] bool parse_double_c(std::string_view token, double& out) {
    std::string buf{token};
    std::istringstream is{buf};
    is.imbue(std::locale::classic());
    double value = 0.0;
    is >> value;
    if (is.fail()) {
        return false;
    }
    // 允许尾随单位词(Hz / dB),但不允许尾随垃圾数字。
    out = value;
    return std::isfinite(value);
}

[[nodiscard]] std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) != 0)) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) == 0)) {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// 关键字后面的第一个可解析数值。按关键字扫描而不是按固定位置取,这样 AutoEq 里
// 省略 Gain 的 LP/HP/BP 行也能正确解析。
[[nodiscard]] bool value_after(const std::vector<std::string_view>& tokens, std::string_view key, double& out) {
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (iequals(tokens[i], key)) {
            return parse_double_c(tokens[i + 1], out);
        }
    }
    return false;
}

[[nodiscard]] bool map_band_type(std::string_view token, HptfBandType& out) {
    if (iequals(token, "PK") || iequals(token, "PEQ") || iequals(token, "MODAL")) {
        out = HptfBandType::peaking;
        return true;
    }
    if (iequals(token, "LSC") || iequals(token, "LS") || iequals(token, "LSQ")) {
        out = HptfBandType::low_shelf;
        return true;
    }
    if (iequals(token, "HSC") || iequals(token, "HS") || iequals(token, "HSQ")) {
        out = HptfBandType::high_shelf;
        return true;
    }
    if (iequals(token, "LP") || iequals(token, "LPQ")) {
        out = HptfBandType::low_pass;
        return true;
    }
    if (iequals(token, "HP") || iequals(token, "HPQ")) {
        out = HptfBandType::high_pass;
        return true;
    }
    if (iequals(token, "BP")) {
        out = HptfBandType::band_pass;
        return true;
    }
    if (iequals(token, "NO")) {
        out = HptfBandType::notch;
        return true;
    }
    return false;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) != 0)) {
        ++b;
    }
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)) {
        --e;
    }
    return s.substr(b, e - b);
}

// 单段 RBJ 系数。搁架用 Q 参数化形式(alpha = sin(w0)/(2Q)),不是 slope S 形式——
// 这才是 EqualizerAPO / AutoEq 的 LSC/HSC 含义。
[[nodiscard]] HptfBiquad design_band(const HptfBand& band, double sample_rate) noexcept {
    const double a_amp = std::pow(10.0, band.gain_db / 40.0);
    const double w0 = 2.0 * std::acos(-1.0) * band.fc_hz / sample_rate;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn / (2.0 * band.q);

    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;

    switch (band.type) {
    case HptfBandType::peaking:
        b0 = 1.0 + (alpha * a_amp);
        b1 = -2.0 * cs;
        b2 = 1.0 - (alpha * a_amp);
        a0 = 1.0 + (alpha / a_amp);
        a1 = -2.0 * cs;
        a2 = 1.0 - (alpha / a_amp);
        break;
    case HptfBandType::low_shelf: {
        const double t = 2.0 * std::sqrt(a_amp) * alpha;
        b0 = a_amp * ((a_amp + 1.0) - ((a_amp - 1.0) * cs) + t);
        b1 = 2.0 * a_amp * ((a_amp - 1.0) - ((a_amp + 1.0) * cs));
        b2 = a_amp * ((a_amp + 1.0) - ((a_amp - 1.0) * cs) - t);
        a0 = (a_amp + 1.0) + ((a_amp - 1.0) * cs) + t;
        a1 = -2.0 * ((a_amp - 1.0) + ((a_amp + 1.0) * cs));
        a2 = (a_amp + 1.0) + ((a_amp - 1.0) * cs) - t;
        break;
    }
    case HptfBandType::high_shelf: {
        const double t = 2.0 * std::sqrt(a_amp) * alpha;
        b0 = a_amp * ((a_amp + 1.0) + ((a_amp - 1.0) * cs) + t);
        b1 = -2.0 * a_amp * ((a_amp - 1.0) + ((a_amp + 1.0) * cs));
        b2 = a_amp * ((a_amp + 1.0) + ((a_amp - 1.0) * cs) - t);
        a0 = (a_amp + 1.0) - ((a_amp - 1.0) * cs) + t;
        a1 = 2.0 * ((a_amp - 1.0) - ((a_amp + 1.0) * cs));
        a2 = (a_amp + 1.0) - ((a_amp - 1.0) * cs) - t;
        break;
    }
    case HptfBandType::low_pass:
        b0 = (1.0 - cs) / 2.0;
        b1 = 1.0 - cs;
        b2 = (1.0 - cs) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cs;
        a2 = 1.0 - alpha;
        break;
    case HptfBandType::high_pass:
        b0 = (1.0 + cs) / 2.0;
        b1 = -(1.0 + cs);
        b2 = (1.0 + cs) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cs;
        a2 = 1.0 - alpha;
        break;
    case HptfBandType::band_pass:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cs;
        a2 = 1.0 - alpha;
        break;
    case HptfBandType::notch:
        b0 = 1.0;
        b1 = -2.0 * cs;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cs;
        a2 = 1.0 - alpha;
        break;
    }

    HptfBiquad out;
    out.b0 = static_cast<float>(b0 / a0);
    out.b1 = static_cast<float>(b1 / a0);
    out.b2 = static_cast<float>(b2 / a0);
    out.a1 = static_cast<float>(a1 / a0);
    out.a2 = static_cast<float>(a2 / a0);
    return out;
}

} // namespace

// ── 解析 ──────────────────────────────────────────────────────────────────────

Result<HptfProfile> parse_parametric_eq(std::string_view text) {
    HptfProfile profile;
    bool saw_any_line = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view raw = (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        const std::string_view line = trim(raw); // 顺带吃掉 CRLF 的 '\r'
        if (line.empty() || line.front() == '#') {
            continue;
        }
        saw_any_line = true;

        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        if (iequals(tokens.front(), "Preamp:") || iequals(tokens.front(), "Preamp")) {
            double preamp = 0.0;
            // "Preamp: -4.1 dB" —— 取第一个可解析的数值
            bool got = false;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                if (parse_double_c(tokens[i], preamp)) {
                    got = true;
                    break;
                }
            }
            if (!got) {
                return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 行数值无法解析", std::string{line});
            }
            profile.preamp_db = preamp;
            continue;
        }

        if (!iequals(tokens.front(), "Filter")) {
            continue; // 未知行:忽略
        }
        // Filter N: ON|OFF TYPE Fc <f> Hz Gain <g> dB Q <q>
        std::size_t idx = 1;
        while (idx < tokens.size() && !iequals(tokens[idx], "ON") && !iequals(tokens[idx], "OFF")) {
            ++idx;
        }
        if (idx >= tokens.size()) {
            continue; // 没有 ON/OFF:不是一条滤波器行
        }
        const bool enabled = iequals(tokens[idx], "ON");
        if (idx + 1 >= tokens.size()) {
            continue; // 只有 ON/OFF,没有类型(AutoEq 对空槽会这么写)
        }

        HptfBandType type{};
        if (!map_band_type(tokens[idx + 1], type)) {
            continue; // 未知类型:跳过这一段(AutoEq 的类型词表会漂移)
        }

        HptfBand band;
        band.type = type;
        band.enabled = enabled;

        double fc = 0.0;
        if (!value_after(tokens, "Fc", fc)) {
            return make_error(ErrorCode::invalid_argument, "HpTF:滤波器行缺少可解析的 Fc", std::string{line});
        }
        band.fc_hz = fc;

        double gain = 0.0;
        band.gain_db = value_after(tokens, "Gain", gain) ? gain : 0.0;

        double q = 0.0;
        band.q = value_after(tokens, "Q", q) ? q : k_default_q;

        if (!(band.fc_hz > 0.0) || !std::isfinite(band.fc_hz)) {
            return make_error(ErrorCode::invalid_argument, "HpTF:Fc 必须是有限正数", std::string{line});
        }
        if (!(band.q > 0.0) || !std::isfinite(band.q)) {
            return make_error(ErrorCode::invalid_argument, "HpTF:Q 必须是有限正数", std::string{line});
        }
        if (!std::isfinite(band.gain_db)) {
            return make_error(ErrorCode::invalid_argument, "HpTF:Gain 必须是有限数", std::string{line});
        }

        profile.bands.push_back(band);
    }

    if (!saw_any_line) {
        return make_error(ErrorCode::invalid_argument, "HpTF:ParametricEQ 内容为空", {});
    }

    const auto enabled_count = static_cast<std::size_t>(
        std::count_if(profile.bands.begin(), profile.bands.end(), [](const HptfBand& b) { return b.enabled; }));
    if (enabled_count > k_hptf_max_bands) {
        return make_error(ErrorCode::invalid_argument,
                          "HpTF:启用的滤波器段数超过上限 " + std::to_string(k_hptf_max_bands),
                          "count=" + std::to_string(enabled_count));
    }

    if (!std::isfinite(profile.preamp_db)) {
        return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 必须是有限数", {});
    }
    return profile;
}

Result<HptfProfile> load_parametric_eq_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return make_error(ErrorCode::io_error, "HpTF:无法打开 ParametricEQ 文件", "path=" + path.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = parse_parametric_eq(buf.str());
    if (!parsed) {
        auto err = parsed.error();
        if (err.context.empty()) {
            err.context = "path=" + path.string();
        } else {
            err.context += " path=" + path.string();
        }
        return tl::unexpected{std::move(err)};
    }
    parsed->name = path.filename().string();
    return parsed;
}

// ── 设计 ──────────────────────────────────────────────────────────────────────

double cascade_magnitude_db(const HptfCoefficients& coeffs, double hz) {
    if (coeffs.sample_rate == 0) {
        return 0.0;
    }
    const double w = 2.0 * std::acos(-1.0) * hz / static_cast<double>(coeffs.sample_rate);
    const std::complex<double> z{std::cos(-w), std::sin(-w)};
    const std::complex<double> z2 = z * z;

    double total_db = 20.0 * std::log10(static_cast<double>(coeffs.preamp_gain));
    for (std::uint32_t i = 0; i < coeffs.band_count; ++i) {
        const auto& s = coeffs.sections.at(i);
        const std::complex<double> num =
            static_cast<double>(s.b0) + (static_cast<double>(s.b1) * z) + (static_cast<double>(s.b2) * z2);
        const std::complex<double> den = 1.0 + (static_cast<double>(s.a1) * z) + (static_cast<double>(s.a2) * z2);
        const double mag = std::abs(num) / std::max(std::abs(den), 1e-30);
        total_db += 20.0 * std::log10(std::max(mag, 1e-30));
    }
    return total_db;
}

Result<HptfCoefficients> design_cascade(const HptfProfile& profile, std::uint32_t sample_rate, HptfPreampMode mode) {
    if (sample_rate == 0) {
        return make_error(ErrorCode::invalid_argument, "HpTF:采样率必须为正", {});
    }

    HptfCoefficients out;
    out.sample_rate = sample_rate;

    const double nyquist = static_cast<double>(sample_rate) * 0.5;
    std::uint32_t count = 0;
    for (const auto& band : profile.bands) {
        if (!band.enabled) {
            continue;
        }
        // fc 越界的段在**设计期**跳过:20 kHz 段在 48 kHz 合法、在 32 kHz 不合法。
        if (band.fc_hz <= 0.0 || band.fc_hz >= nyquist) {
            continue;
        }
        if (count >= k_hptf_max_bands) {
            return make_error(
                ErrorCode::invalid_argument, "HpTF:启用的滤波器段数超过上限 " + std::to_string(k_hptf_max_bands), {});
        }
        out.sections.at(count) = design_band(band, static_cast<double>(sample_rate));
        ++count;
    }
    out.band_count = count;
    out.preamp_gain = static_cast<float>(std::pow(10.0, profile.preamp_db / 20.0));

    // 合成响应峰值:对数网格扫描。
    const double high = std::min(k_scan_high_hz, nyquist * 0.98);
    double peak_db = -1000.0;
    if (high > k_scan_low_hz && count > 0) {
        const double ratio = std::log(high / k_scan_low_hz) / static_cast<double>(k_scan_points - 1);
        for (int i = 0; i < k_scan_points; ++i) {
            const double hz = k_scan_low_hz * std::exp(ratio * static_cast<double>(i));
            peak_db = std::max(peak_db, cascade_magnitude_db(out, hz));
        }
    } else {
        peak_db = 20.0 * std::log10(static_cast<double>(out.preamp_gain));
    }

    if (mode == HptfPreampMode::auto_trim && peak_db > 0.0) {
        out.auto_trim_db = static_cast<float>(-peak_db);
        out.preamp_gain = static_cast<float>(static_cast<double>(out.preamp_gain) * std::pow(10.0, -peak_db / 20.0));
        peak_db = 0.0;
    }
    out.max_response_db = static_cast<float>(peak_db);
    return out;
}

// ── HptfCascade ───────────────────────────────────────────────────────────────

void HptfCascade::prepare(std::uint32_t channels) {
    channels_ = channels;
    state_.assign(static_cast<std::size_t>(channels) * k_hptf_max_bands * 2U, 0.0);
}

void HptfCascade::set_coefficients(const HptfCoefficients& coeffs) noexcept {
    coeffs_ = coeffs;
}

void HptfCascade::reset() noexcept {
    std::fill(state_.begin(), state_.end(), 0.0);
}

void HptfCascade::process(float* interleaved, std::size_t frames) noexcept {
    // bypass 是**精确短路**:不做 x * 1.0f,于是不启用 HpTF 时逐样本 bit-identical。
    if (frames == 0 || channels_ == 0 || coeffs_.band_count == 0 || interleaved == nullptr) {
        return;
    }
    const std::uint32_t bands = coeffs_.band_count;
    const double preamp = static_cast<double>(coeffs_.preamp_gain);

    for (std::uint32_t ch = 0; ch < channels_; ++ch) {
        double* st = state_.data() + (static_cast<std::size_t>(ch) * k_hptf_max_bands * 2U);
        for (std::size_t f = 0; f < frames; ++f) {
            double x = static_cast<double>(interleaved[(f * channels_) + ch]) * preamp;
            for (std::uint32_t b = 0; b < bands; ++b) {
                const auto& s = coeffs_.sections.at(b);
                double* z = st + (static_cast<std::size_t>(b) * 2U);
                // 转置直接 II 型。括号固定运算顺序(不覆盖 fast-math 类重排选项)。
                const double y = (static_cast<double>(s.b0) * x) + z[0];
                z[0] = ((static_cast<double>(s.b1) * x) - (static_cast<double>(s.a1) * y)) + z[1];
                z[1] = (static_cast<double>(s.b2) * x) - (static_cast<double>(s.a2) * y);
                x = y;
            }
            interleaved[(f * channels_) + ch] = static_cast<float>(x);
        }
    }

    // 每块一次的状态体检(不是每样本)。
    // 1) 一个 NaN 会永久毒化 IIR —— 发现非有限值就整体清零。
    // 2) 高 Q 低频段衰减到静音后会长时间停在非正规数区间,某些 x86 路径上代价可达 ~100×;
    //    全体低于阈值即清零。不动 FTZ/DAZ —— pull() 跑在宿主拥有的线程上。
    bool non_finite = false;
    bool all_tiny = true;
    for (const double v : state_) {
        if (!std::isfinite(v)) {
            non_finite = true;
            break;
        }
        if (std::fabs(v) > 1e-20) {
            all_tiny = false;
        }
    }
    if (non_finite || all_tiny) {
        std::fill(state_.begin(), state_.end(), 0.0);
    }
}

// ── HptfProcessor ─────────────────────────────────────────────────────────────

void HptfProcessor::prepare(std::uint32_t channels, std::uint32_t sample_rate) {
    channels_ = channels;
    sample_rate_ = sample_rate;
    front_.prepare(channels);
    back_.prepare(channels);
    scratch_.assign(k_hptf_chunk_frames * static_cast<std::size_t>(std::max<std::uint32_t>(channels, 1U)), 0.0F);
}

void HptfProcessor::publish(const HptfCoefficients& coeffs, std::uint64_t revision) {
    const std::uint32_t g = ++generation_;
    const std::size_t slot = g & 1U;
    // 写者可以阻塞,回调不可以。等回调退出拷贝窗口(最多一次约 300 字节的 memcpy)。
    while (in_copy_.load(std::memory_order_seq_cst) != 0U) {
        std::this_thread::yield();
    }
    slots_.at(slot) = coeffs;
    slot_revisions_.at(slot) = revision;
    pending_.store(g, std::memory_order_release); // release 发布上面整个结构体镜像
}

void HptfProcessor::publish_bypass(std::uint64_t revision) {
    HptfCoefficients bypass;
    bypass.sample_rate = sample_rate_;
    bypass.band_count = 0;
    publish(bypass, revision);
}

void HptfProcessor::reset_state() noexcept {
    front_.reset();
    back_.reset();
    blending_ = false;
    blend_pos_ = 0;
}

std::uint64_t HptfProcessor::applied_revision() const noexcept {
    return applied_revision_.load(std::memory_order_relaxed);
}

HptfCoefficients HptfProcessor::active_coefficients() const noexcept {
    return front_.coefficients();
}

void HptfProcessor::finish_blend() noexcept {
    std::swap(front_, back_);
    blending_ = false;
    blend_pos_ = 0;
    applied_revision_.store(blend_revision_, std::memory_order_relaxed);
}

void HptfProcessor::process(float* interleaved, std::size_t frames) noexcept {
    if (frames == 0 || channels_ == 0 || interleaved == nullptr) {
        return;
    }

    // 取走待处理的发布。混合期间不接新的:同一时刻只允许一次 swap 在飞,于是绝不
    // 三方混合、绝不中途硬切;混合结束后下一次 process 自然接上排队的那份。
    const std::uint32_t g = pending_.load(std::memory_order_acquire);
    if (g != consumed_generation_ && !blending_) {
        in_copy_.store(1U, std::memory_order_seq_cst);
        const std::size_t slot = g & 1U;
        back_.set_coefficients(slots_.at(slot));
        blend_revision_ = slot_revisions_.at(slot);
        in_copy_.store(0U, std::memory_order_seq_cst);
        back_.reset(); // 切进来的那条从零状态起步;它的权重此刻正好是 0
        consumed_generation_ = g;
        blending_ = true;
        blend_pos_ = 0;
    }

    const std::size_t chunk = k_hptf_chunk_frames;
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(frames - done, chunk);
        float* p = interleaved + (done * channels_);

        if (!blending_) {
            front_.process(p, n);
            done += n;
            continue;
        }

        const std::size_t floats = n * channels_;
        std::copy_n(p, floats, scratch_.data());
        front_.process(p, n);              // 旧
        back_.process(scratch_.data(), n); // 新

        // 线性混合。两条级联吃同一输入、输出强相关,等功率律会在中点鼓出约 +3 dB。
        for (std::size_t f = 0; f < n; ++f) {
            const double pos = static_cast<double>(blend_pos_ + f);
            const float t = static_cast<float>(std::min(1.0, pos / static_cast<double>(k_hptf_blend_frames)));
            for (std::uint32_t c = 0; c < channels_; ++c) {
                const std::size_t i = (f * channels_) + c;
                p[i] = (p[i] * (1.0F - t)) + (scratch_[i] * t);
            }
        }
        blend_pos_ += n;
        if (blend_pos_ >= k_hptf_blend_frames) {
            finish_blend();
        }
        done += n;
    }
}

} // namespace mradm::render_common
