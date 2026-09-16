#include "hptf_eq.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <fstream>
#include <ios>
#include <limits>
#include <locale>
#include <queue>
#include <sstream>
#include <utility>

namespace mradm::render_common {
namespace {

constexpr double k_default_q = 0.707;

// auto_trim / max_response 的搜索范围:可听带 20 Hz – 20 kHz(上限夹到 Nyquist 以下)。
//
// 刻意不往 20 Hz 以下扫。以 MDR-MV1 为例,+9.7 dB 的 105 Hz 低架在 46 Hz 凹陷影响消退后
// 会一路抬起来:20 Hz 处 -0.52 dB,10 Hz 处 +2.93 dB,5 Hz 处 +4.75 dB。若把次声区计入峰值,
// auto_trim 会为了保护无耳机能重放、也几乎不存在于节目里的频率,把整条曲线再压掉近 5 dB。
// AutoEq 自己的 Preamp 也是按可听带算的,取同一约定才能与它的标称值对得上。
constexpr double k_scan_low_hz = 20.0;
constexpr double k_scan_high_hz = 20000.0;
constexpr double k_peak_tolerance_db = 0.001;
constexpr std::size_t k_peak_refinements = 4096;

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
    // Units are separate tokens in AutoEq; a partial number must not silently change a curve.
    out = value;
    return is.eof() && std::isfinite(value);
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
[[nodiscard]] Result<double>
value_after(const std::vector<std::string_view>& tokens, std::string_view key, double fallback) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (iequals(tokens[i], key)) {
            double value = 0.0;
            if (i + 1 >= tokens.size() || !parse_double_c(tokens[i + 1], value)) {
                return make_error(ErrorCode::invalid_argument, "HpTF:数值无法解析", std::string{key});
            }
            return value;
        }
    }
    return fallback;
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
[[nodiscard]] Result<HptfBiquad> design_band(const HptfBand& band, double sample_rate) {
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

    const std::array normalized{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    if (!std::ranges::all_of(normalized, [](double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        })) {
        return make_error(ErrorCode::invalid_argument, "HpTF:滤波器系数超出可表示范围");
    }
    const HptfBiquad out{static_cast<float>(normalized[0]),
                         static_cast<float>(normalized[1]),
                         static_cast<float>(normalized[2]),
                         static_cast<float>(normalized[3]),
                         static_cast<float>(normalized[4])};
    // Jury stability conditions, checked AFTER coefficient quantisation.
    const double qa1 = out.a1;
    const double qa2 = out.a2;
    if (1.0 + qa1 + qa2 <= 0.0 || 1.0 - qa1 + qa2 <= 0.0 || 1.0 - qa2 <= 0.0) {
        return make_error(ErrorCode::invalid_argument, "HpTF:滤波器在当前采样率下不稳定");
    }
    return out;
}

[[nodiscard]] double section_magnitude_db(const HptfBiquad& section, double hz, double sample_rate) {
    const double w = 2.0 * std::acos(-1.0) * hz / sample_rate;
    const std::complex<double> z{std::cos(-w), std::sin(-w)};
    const auto numerator =
        static_cast<double>(section.b0) + static_cast<double>(section.b1) * z + static_cast<double>(section.b2) * z * z;
    const auto denominator = 1.0 + static_cast<double>(section.a1) * z + static_cast<double>(section.a2) * z * z;
    return 20.0 * std::log10(std::max(std::abs(numerator) / std::max(std::abs(denominator), 1e-30), 1e-30));
}

struct BandExtrema {
    HptfBiquad section;
    std::array<double, 3> frequencies{-1.0, -1.0, -1.0};
};

[[nodiscard]] BandExtrema find_band_extrema(const HptfBiquad& section, double fc, double sample_rate) {
    // Squared magnitude is quadratic in sin(w/2)^2. This shifted form preserves
    // low-frequency precision: expanding in cos(w) subtracts nearly equal O(1)
    // coefficients near DC. The derivative of the polynomial ratio has degree two.
    const auto polynomial = [](long double b0, long double b1, long double b2) {
        const auto sum = b0 + b1 + b2;
        const auto difference = b0 - b2;
        return std::array{sum * sum, 4.0L * ((difference * difference) - (sum * (b0 + b2))), 16.0L * b0 * b2};
    };
    const auto n = polynomial(section.b0, section.b1, section.b2);
    const auto d = polynomial(1.0L, section.a1, section.a2);
    const auto a = (n[2] * d[1]) - (n[1] * d[2]);
    const auto b = 2.0L * ((n[2] * d[0]) - (n[0] * d[2]));
    const auto c = (n[1] * d[0]) - (n[0] * d[1]);
    BandExtrema result{section, {fc, -1.0, -1.0}};
    const auto store_root = [&](long double root, std::size_t index) {
        if (root >= 0.0L && root <= 1.0L) {
            result.frequencies.at(index) =
                static_cast<double>(std::asin(std::sqrt(root))) * sample_rate / std::acos(-1.0);
        }
    };
    if (a == 0.0L) {
        if (b != 0.0L) {
            store_root(-c / b, 1);
        }
    } else if (const auto discriminant = (b * b) - (4.0L * a * c); discriminant >= 0.0L) {
        const auto q = -0.5L * (b + std::copysign(std::sqrt(discriminant), b));
        store_root(q / a, 1);
        if (q != 0.0L) {
            store_root(c / q, 2);
        }
    }
    return result;
}

struct PeakInterval {
    double low;
    double high;
    double upper;
};

[[nodiscard]] double peak_response_db(const HptfCoefficients& coeffs, std::span<const BandExtrema> bands) {
    const double high = std::min(k_scan_high_hz, static_cast<double>(coeffs.sample_rate) * 0.49);
    const double preamp_db = 20.0 * std::log10(static_cast<double>(coeffs.preamp_gain));
    if (high <= k_scan_low_hz || bands.empty()) {
        return preamp_db;
    }
    const auto interval_bound = [&](double low, double upper) {
        double bound = preamp_db;
        for (const auto& band : bands) {
            double peak = std::max(section_magnitude_db(band.section, low, coeffs.sample_rate),
                                   section_magnitude_db(band.section, upper, coeffs.sample_rate));
            for (const double hz : band.frequencies) {
                if (hz >= low && hz <= upper) {
                    peak = std::max(peak, section_magnitude_db(band.section, hz, coeffs.sample_rate));
                }
            }
            bound += peak;
        }
        return bound;
    };
    double measured = std::max(cascade_magnitude_db(coeffs, k_scan_low_hz), cascade_magnitude_db(coeffs, high));
    for (const auto& band : bands) {
        for (const double hz : band.frequencies) {
            if (hz >= k_scan_low_hz && hz <= high) {
                measured = std::max(measured, cascade_magnitude_db(coeffs, hz));
            }
        }
    }
    const auto compare = [](const PeakInterval& left, const PeakInterval& right) { return left.upper < right.upper; };
    std::priority_queue<PeakInterval, std::vector<PeakInterval>, decltype(compare)> pending(compare);
    pending.push({k_scan_low_hz, high, interval_bound(k_scan_low_hz, high)});
    for (std::size_t i = 0; i < k_peak_refinements && pending.top().upper > measured + k_peak_tolerance_db; ++i) {
        const auto interval = pending.top();
        pending.pop();
        const double middle = std::sqrt(interval.low * interval.high);
        measured = std::max(measured, cascade_magnitude_db(coeffs, middle));
        pending.push({interval.low, middle, interval_bound(interval.low, middle)});
        pending.push({middle, interval.high, interval_bound(middle, interval.high)});
    }
    // If the work budget is exhausted, keep the remaining conservative bound.
    const double bound = std::max(measured, pending.top().upper);
    return bound > 0.0 ? bound + k_peak_tolerance_db : bound;
}

} // namespace

// ── 解析 ──────────────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-size)
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

        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        if (iequals(tokens.front(), "Preamp:") || iequals(tokens.front(), "Preamp")) {
            double preamp = 0.0;
            if (tokens.size() < 2 || !parse_double_c(tokens[1], preamp)) {
                return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 行数值无法解析", std::string{line});
            }
            profile.preamp_db = preamp;
            saw_any_line = true;
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

        const auto fc = value_after(tokens, "Fc", 0.0);
        if (!fc) {
            return make_error(ErrorCode::invalid_argument, "HpTF:滤波器行缺少可解析的 Fc", std::string{line});
        }
        band.fc_hz = *fc;

        const auto gain = value_after(tokens, "Gain", 0.0);
        const auto q = value_after(tokens, "Q", k_default_q);
        if (!gain || !q) {
            return make_error(ErrorCode::invalid_argument, "HpTF:Gain 或 Q 数值无法解析", std::string{line});
        }
        band.gain_db = *gain;
        band.q = *q;

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
        saw_any_line = true;
    }

    if (!saw_any_line) {
        return make_error(ErrorCode::invalid_argument, "HpTF:没有可用的 ParametricEQ 参数", {});
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
    if (mode != HptfPreampMode::warn_only && mode != HptfPreampMode::auto_trim) {
        return make_error(ErrorCode::invalid_argument, "HpTF:未知的前级策略");
    }
    const double gain = std::pow(10.0, profile.preamp_db / 20.0);
    if (!std::isfinite(profile.preamp_db) || !std::isfinite(gain) || gain > std::numeric_limits<float>::max() ||
        gain < std::numeric_limits<float>::min()) {
        return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 超出可表示范围");
    }

    HptfCoefficients out;
    out.sample_rate = sample_rate;
    out.preamp_gain = static_cast<float>(gain);
    out.preamp_db = static_cast<float>(profile.preamp_db);
    std::array<BandExtrema, k_hptf_max_bands> extrema;

    const double nyquist = static_cast<double>(sample_rate) * 0.5;
    std::uint32_t count = 0;
    for (const auto& band : profile.bands) {
        if (!band.enabled) {
            continue;
        }
        if (!std::isfinite(band.fc_hz) || band.fc_hz <= 0.0 || !std::isfinite(band.q) || band.q <= 0.0 ||
            !std::isfinite(band.gain_db)) {
            return make_error(ErrorCode::invalid_argument, "HpTF:滤波器参数必须为有限数且 Fc/Q 为正");
        }
        // fc 越界的段在**设计期**跳过:20 kHz 段在 48 kHz 合法、在 32 kHz 不合法。
        if (band.fc_hz <= 0.0 || band.fc_hz >= nyquist) {
            continue;
        }
        if (count >= k_hptf_max_bands) {
            return make_error(
                ErrorCode::invalid_argument, "HpTF:启用的滤波器段数超过上限 " + std::to_string(k_hptf_max_bands), {});
        }
        const auto section = design_band(band, static_cast<double>(sample_rate));
        if (!section) {
            return tl::unexpected{section.error()};
        }
        out.sections.at(count) = *section;
        extrema.at(count) = find_band_extrema(*section, band.fc_hz, sample_rate);
        ++count;
    }
    out.band_count = count;
    double peak_db = peak_response_db(out, std::span{extrema}.first(count));
    if (!std::isfinite(peak_db) || peak_db > 20.0 * std::log10(std::numeric_limits<float>::max())) {
        return make_error(ErrorCode::invalid_argument, "HpTF:级联响应超出可表示范围");
    }

    if (mode == HptfPreampMode::auto_trim && peak_db > 0.0) {
        const double trimmed = static_cast<double>(out.preamp_gain) * std::pow(10.0, -peak_db / 20.0);
        if (!std::isfinite(trimmed) || trimmed < std::numeric_limits<float>::min()) {
            return make_error(ErrorCode::invalid_argument, "HpTF:自动衰减后的增益超出可表示范围");
        }
        const float trimmed_gain = std::nextafter(static_cast<float>(trimmed), 0.0F);
        const double adjustment = 20.0 * std::log10(static_cast<double>(trimmed_gain) / out.preamp_gain);
        out.auto_trim_db = static_cast<float>(adjustment);
        out.preamp_gain = trimmed_gain;
        peak_db += adjustment;
    }
    out.max_response_db = static_cast<float>(peak_db);
    return out;
}

// ── HptfCascade ───────────────────────────────────────────────────────────────

void HptfCascade::prepare(std::uint32_t channel_count) {
    channels_ = channel_count;
    state_.assign(static_cast<std::size_t>(channel_count) * k_hptf_max_bands * 2U, 0.0);
}

void HptfCascade::set_coefficients(const HptfCoefficients& coeffs) noexcept {
    coeffs_ = coeffs;
}

void HptfCascade::reset() noexcept {
    std::ranges::fill(state_, 0.0);
}

void HptfCascade::process(float* interleaved, std::size_t frames) noexcept {
    // bypass 是**精确短路**:不做 x * 1.0f,于是不启用 HpTF 时逐样本 bit-identical。
    if (frames == 0 || channels_ == 0 || is_bypass() || interleaved == nullptr) {
        return;
    }
    const std::uint32_t bands = coeffs_.band_count;
    const auto preamp = static_cast<double>(coeffs_.preamp_gain);

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
        std::ranges::fill(state_, 0.0);
    }
}

// ── HptfProcessor ─────────────────────────────────────────────────────────────

void HptfMailbox::publish(const HptfSnapshot& snapshot) noexcept {
    slots_.at(write_slot_) = snapshot;
    write_slot_ = middle_.exchange(write_slot_ | k_dirty, std::memory_order_acq_rel) & k_index_mask;
}

std::optional<HptfSnapshot> HptfMailbox::consume() noexcept {
    if ((middle_.load(std::memory_order_acquire) & k_dirty) == 0U) {
        return std::nullopt;
    }
    read_slot_ = middle_.exchange(read_slot_, std::memory_order_acq_rel) & k_index_mask;
    return slots_.at(read_slot_);
}

void HptfProcessor::prepare(std::uint32_t channel_count, std::uint32_t rate) {
    channels_ = channel_count;
    sample_rate_ = rate;
    front_.prepare(channel_count);
    back_.prepare(channel_count);
    HptfCoefficients bypass;
    bypass.sample_rate = rate;
    front_.set_coefficients(bypass);
    status_.publish({bypass, 0});
    scratch_.assign(k_hptf_chunk_frames * static_cast<std::size_t>(std::max<std::uint32_t>(channel_count, 1U)), 0.0F);
}

void HptfProcessor::publish(const HptfCoefficients& coeffs, std::uint64_t revision) {
    const std::lock_guard lock(publication_mutex_);
    pending_.publish({coeffs, revision});
}

void HptfProcessor::publish_bypass(std::uint64_t revision) {
    HptfCoefficients bypass;
    bypass.sample_rate = sample_rate_;
    bypass.band_count = 0;
    publish(bypass, revision);
}

void HptfProcessor::reset_state() noexcept {
    if (const auto target = pending_.consume()) {
        front_.set_coefficients(target->coefficients);
        front_revision_ = target->revision;
    } else if (blending_) {
        std::swap(front_, back_);
        front_revision_ = blend_revision_;
    }
    front_.reset();
    back_.reset();
    blending_ = false;
    blend_pos_ = 0;
    status_.publish({front_.coefficients(), front_revision_});
}

HptfSnapshot HptfProcessor::active_snapshot() const {
    const std::lock_guard lock(status_mutex_);
    if (const auto latest = status_.consume()) {
        observed_ = *latest;
    }
    return observed_;
}

std::uint64_t HptfProcessor::applied_revision() const {
    return active_snapshot().revision;
}

HptfCoefficients HptfProcessor::active_coefficients() const {
    return active_snapshot().coefficients;
}

void HptfProcessor::finish_blend() noexcept {
    std::swap(front_, back_);
    blending_ = false;
    blend_pos_ = 0;
    front_revision_ = blend_revision_;
    status_.publish({front_.coefficients(), front_revision_});
}

void HptfProcessor::process(float* interleaved, std::size_t frames) noexcept {
    if (frames == 0 || channels_ == 0 || interleaved == nullptr) {
        return;
    }

    // 取走待处理的发布。混合期间不接新的:同一时刻只允许一次 swap 在飞,于是绝不
    // 三方混合、绝不中途硬切;混合结束后下一次 process 自然接上排队的那份。
    if (!blending_) {
        if (const auto target = pending_.consume()) {
            back_.set_coefficients(target->coefficients);
            blend_revision_ = target->revision;
            back_.reset();
            blending_ = true;
            blend_pos_ = 0;
        }
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
            const auto pos = static_cast<double>(blend_pos_ + f);
            const auto t = static_cast<float>(std::min(1.0, pos / static_cast<double>(k_hptf_blend_frames)));
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
