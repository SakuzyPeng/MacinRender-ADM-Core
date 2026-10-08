// Frozen test-only AutoEq ParametricEQ parser; see provenance.json.
#pragma once
#include <cctype>
#include <cmath>
#include <cstddef>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/hptf.h"

namespace mradm::hptf_parse_legacy {
using mradm::ErrorCode;
using mradm::HptfBand;
using mradm::HptfBandType;
using mradm::HptfProfile;
using mradm::k_hptf_max_bands;
using mradm::make_error;
using mradm::Result;
namespace detail {

constexpr double k_default_q = 0.707;

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
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
[[nodiscard]] inline bool parse_double_c(std::string_view token, double& out) {
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

[[nodiscard]] inline std::vector<std::string_view> tokenize(std::string_view line) {
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
[[nodiscard]] inline Result<double>
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

[[nodiscard]] inline bool map_band_type(std::string_view token, HptfBandType& out) {
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

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
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

} // namespace detail
using namespace detail;

inline Result<void> validate_hptf_profile(const HptfProfile& profile) {
    if (!std::isfinite(profile.preamp_db)) {
        return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 必须是有限数");
    }
    std::size_t enabled_count = 0;
    for (std::size_t index = 0; index < profile.bands.size(); ++index) {
        const auto& band = profile.bands[index];
        if (band.type > HptfBandType::notch || !std::isfinite(band.fc_hz) || band.fc_hz <= 0.0 ||
            !std::isfinite(band.q) || band.q <= 0.0 || !std::isfinite(band.gain_db)) {
            return make_error(ErrorCode::invalid_argument,
                              "HpTF:滤波器类型必须有效,参数必须为有限数且 Fc/Q 为正",
                              "band=" + std::to_string(index + 1));
        }
        if (band.enabled && ++enabled_count > k_hptf_max_bands) {
            return make_error(ErrorCode::invalid_argument,
                              "HpTF:启用的滤波器段数超过上限 " + std::to_string(k_hptf_max_bands));
        }
    }
    return {};
}

// NOLINTNEXTLINE(readability-function-size)
inline Result<HptfProfile> parse_parametric_eq(std::string_view text) {
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

        profile.bands.push_back(band);
        saw_any_line = true;
    }

    if (!saw_any_line) {
        return make_error(ErrorCode::invalid_argument, "HpTF:没有可用的 ParametricEQ 参数", {});
    }

    if (auto valid = validate_hptf_profile(profile); !valid) {
        return tl::unexpected{valid.error()};
    }
    return profile;
}

} // namespace mradm::hptf_parse_legacy
