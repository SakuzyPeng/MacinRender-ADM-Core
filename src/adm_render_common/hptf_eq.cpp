#include "hptf_eq.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

#include "dsp.h"

namespace mradm::render_common {
namespace {

constexpr double k_default_q = 0.707;

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

static_assert(k_hptf_max_bands == 32U);
static_assert(static_cast<std::uint32_t>(HptfBandType::peaking) == 0U);
static_assert(static_cast<std::uint32_t>(HptfBandType::low_shelf) == 1U);
static_assert(static_cast<std::uint32_t>(HptfBandType::high_shelf) == 2U);
static_assert(static_cast<std::uint32_t>(HptfBandType::low_pass) == 3U);
static_assert(static_cast<std::uint32_t>(HptfBandType::high_pass) == 4U);
static_assert(static_cast<std::uint32_t>(HptfBandType::band_pass) == 5U);
static_assert(static_cast<std::uint32_t>(HptfBandType::notch) == 6U);
static_assert(static_cast<std::uint32_t>(HptfPreampMode::warn_only) == 0U);
static_assert(static_cast<std::uint32_t>(HptfPreampMode::auto_trim) == 1U);

MradmDspHptfCoefficients to_rust(const HptfCoefficients& c) noexcept {
    MradmDspHptfCoefficients result{};
    result.sample_rate = c.sample_rate;
    result.band_count = c.band_count;
    result.preamp_gain = c.preamp_gain;
    result.max_response_db = c.max_response_db;
    result.auto_trim_db = c.auto_trim_db;
    result.preamp_db = c.preamp_db;
    for (std::size_t i = 0; i < k_hptf_max_bands; ++i) {
        const auto& s = c.sections.at(i);
        std::span{result.sections}[i] = {s.b0, s.b1, s.b2, s.a1, s.a2};
    }
    return result;
}
HptfCoefficients from_rust(const MradmDspHptfCoefficients& c) noexcept {
    HptfCoefficients result;
    result.sample_rate = c.sample_rate;
    result.band_count = c.band_count;
    result.preamp_gain = c.preamp_gain;
    result.max_response_db = c.max_response_db;
    result.auto_trim_db = c.auto_trim_db;
    result.preamp_db = c.preamp_db;
    for (std::size_t i = 0; i < k_hptf_max_bands; ++i) {
        const auto& s = std::span{c.sections}[i];
        result.sections.at(i) = {s.b0, s.b1, s.b2, s.a1, s.a2};
    }
    return result;
}
std::optional<MradmDspHptfSnapshot> to_rust(const std::optional<HptfSnapshot>& value) noexcept {
    if (!value) {
        return std::nullopt;
    }
    return MradmDspHptfSnapshot{to_rust(value->coefficients), value->revision};
}
HptfSnapshot from_rust(const MradmDspHptfSnapshot& value) noexcept {
    return {from_rust(value.coefficients), value.revision};
}
// Targets have already been designed on the control thread. An internal
// contract failure during a noexcept callback is a programming error (ADR 0005).
void check_realtime(int status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}
std::size_t audio_length(std::size_t frames, std::uint32_t channels) noexcept {
    if (channels == 0U || frames > std::numeric_limits<std::size_t>::max() / channels) {
        std::terminate();
    }
    return frames * channels;
}

} // namespace

// ── 解析 ──────────────────────────────────────────────────────────────────────

Result<void> validate_hptf_profile(const HptfProfile& profile) {
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
    const auto packed_coefficients = to_rust(coeffs);
    double result = 0.0;
    std::array<char, 256> message{};
    dsp::check(mradm_dsp_hptf_magnitude(&packed_coefficients, hz, &result, message.data(), message.size()),
               message.data());
    return result;
}

Result<HptfCoefficients> design_cascade(const HptfProfile& profile, std::uint32_t sample_rate, HptfPreampMode mode) {
    if (sample_rate == 0U) {
        return make_error(ErrorCode::invalid_argument, "HpTF:采样率必须为正");
    }
    if (auto valid = validate_hptf_profile(profile); !valid) {
        return tl::unexpected{valid.error()};
    }
    std::vector<MradmDspHptfBand> bands;
    bands.reserve(profile.bands.size());
    std::ranges::transform(profile.bands, std::back_inserter(bands), [](const HptfBand& band) {
        return MradmDspHptfBand{
            static_cast<std::uint32_t>(band.type), band.enabled ? 1U : 0U, band.fc_hz, band.gain_db, band.q};
    });
    MradmDspHptfCoefficients result{};
    std::array<char, 256> message{};
    const auto status = mradm_dsp_hptf_design(bands.data(),
                                              bands.size(),
                                              profile.preamp_db,
                                              sample_rate,
                                              static_cast<std::uint32_t>(mode),
                                              &result,
                                              message.data(),
                                              message.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), message.data(), "HpTF design");
    }
    return from_rust(result);
}

// ── HptfCascade ───────────────────────────────────────────────────────────────

struct HptfCascade::State {
    std::unique_ptr<void, decltype(&mradm_dsp_hptf_cascade_destroy)> handle{nullptr, mradm_dsp_hptf_cascade_destroy};
};
HptfCascade::HptfCascade() = default;
HptfCascade::~HptfCascade() = default;
HptfCascade::HptfCascade(HptfCascade&& other) noexcept = default;
HptfCascade& HptfCascade::operator=(HptfCascade&& other) noexcept = default;
HptfCascade::HptfCascade(const HptfCascade& other) : coeffs_(other.coeffs_), channels_(other.channels_) {
    if (other.state_) {
        state_ = std::make_unique<State>();
        void* raw = nullptr;
        std::array<char, 256> message{};
        dsp::check(mradm_dsp_hptf_cascade_clone(other.state_->handle.get(), &raw, message.data(), message.size()),
                   message.data());
        state_->handle.reset(raw);
    }
}
HptfCascade& HptfCascade::operator=(const HptfCascade& other) {
    if (this != &other) {
        HptfCascade copied(other);
        *this = std::move(copied);
    }
    return *this;
}
void HptfCascade::prepare(std::uint32_t channel_count) {
    auto prepared = std::make_unique<State>();
    void* raw = nullptr;
    std::array<char, 256> message{};
    dsp::check(mradm_dsp_hptf_cascade_create(channel_count, &raw, message.data(), message.size()), message.data());
    prepared->handle.reset(raw);
    const auto packed_coefficients = to_rust(coeffs_);
    dsp::check(mradm_dsp_hptf_cascade_set(raw, &packed_coefficients, message.data(), message.size()), message.data());
    state_ = std::move(prepared);
    channels_ = channel_count;
}
void HptfCascade::set_coefficients(const HptfCoefficients& coeffs) noexcept {
    const auto packed_coefficients = to_rust(coeffs);
    if (state_) {
        check_realtime(mradm_dsp_hptf_cascade_set(state_->handle.get(), &packed_coefficients, nullptr, 0U));
    }
    coeffs_ = coeffs;
}
void HptfCascade::reset() noexcept {
    if (state_) {
        check_realtime(mradm_dsp_hptf_cascade_reset(state_->handle.get(), nullptr, 0U));
    }
}
void HptfCascade::process(float* interleaved, std::size_t frames) noexcept {
    if (frames == 0U || channels_ == 0U || !state_ || is_bypass() || interleaved == nullptr) {
        return;
    }
    check_realtime(mradm_dsp_hptf_cascade_process(
        state_->handle.get(), interleaved, audio_length(frames, channels_), nullptr, 0U));
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

struct HptfProcessor::State {
    std::unique_ptr<void, decltype(&mradm_dsp_hptf_processor_destroy)> handle{nullptr,
                                                                              mradm_dsp_hptf_processor_destroy};
    bool blending{false};
};
HptfProcessor::HptfProcessor() = default;
HptfProcessor::~HptfProcessor() = default;

void HptfProcessor::prepare(std::uint32_t channel_count, std::uint32_t rate) {
    auto prepared = std::make_unique<State>();
    void* raw = nullptr;
    std::array<char, 256> message{};
    dsp::check(mradm_dsp_hptf_processor_create(channel_count, rate, &raw, message.data(), message.size()),
               message.data());
    prepared->handle.reset(raw);
    state_ = std::move(prepared);
    channels_ = channel_count;
    sample_rate_ = rate;
    HptfCoefficients bypass;
    bypass.sample_rate = rate;
    status_.publish({bypass, 0});
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
    if (!state_) {
        return;
    }
    const auto target = to_rust(pending_.consume());
    MradmDspHptfUpdate update{};
    check_realtime(
        mradm_dsp_hptf_processor_reset(state_->handle.get(), target ? &*target : nullptr, &update, nullptr, 0U));
    state_->blending = update.blending != 0U;
    status_.publish(from_rust(update.snapshot));
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

void HptfProcessor::process(float* interleaved, std::size_t frames) noexcept {
    if (frames == 0U || channels_ == 0U || !state_ || interleaved == nullptr) {
        return;
    }
    // While a fade is active, the existing mailbox coalesces newer targets.
    // Only the next process call after completion can start the queued fade.
    const auto target = to_rust(state_->blending ? std::nullopt : pending_.consume());
    MradmDspHptfUpdate update{};
    check_realtime(mradm_dsp_hptf_processor_process(state_->handle.get(),
                                                    interleaved,
                                                    audio_length(frames, channels_),
                                                    target ? &*target : nullptr,
                                                    &update,
                                                    nullptr,
                                                    0U));
    state_->blending = update.blending != 0U;
    if (update.applied != 0U) {
        status_.publish(from_rust(update.snapshot));
    }
}

} // namespace mradm::render_common

namespace mradm {
Result<HptfProfile> parse_hptf_parametric_eq(std::string_view text) {
    return render_common::parse_parametric_eq(text);
}
} // namespace mradm
