#include "hptf_eq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <utility>

#include "consistency_trace.h"
#include "dsp.h"

namespace mradm::render_common {
namespace {

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

// AutoEq text is user input; the grammar lives in Rust (mradm-dsp hptf::parametric_eq).
// Error messages and line context stay here, unchanged.
Result<HptfProfile> parse_parametric_eq(std::string_view text) {
    std::vector<MradmDspHptfBand> bands(16);
    MradmDspHptfParseResult result{};
    for (;;) {
        std::array<char, 256> message{};
        if (mradm_dsp_hptf_parse(reinterpret_cast<const std::uint8_t*>(text.data()),
                                 text.size(),
                                 bands.data(),
                                 bands.size(),
                                 &result,
                                 message.data(),
                                 message.size()) != 0) {
            return make_error(ErrorCode::internal_error, "HpTF:ParametricEQ 解析失败", message.data());
        }
        if (result.error != 0U || result.band_count <= bands.size()) {
            break;
        }
        bands.resize(result.band_count);
    }
    const auto line = [&] {
        if (result.line_offset > text.size() || result.line_len > text.size() - result.line_offset) {
            return std::string{};
        }
        return std::string{text.substr(result.line_offset, result.line_len)};
    };
    switch (result.error) {
    case 0:
        break;
    case 1:
        return make_error(ErrorCode::invalid_argument, "HpTF:Preamp 行数值无法解析", line());
    case 2:
        return make_error(ErrorCode::invalid_argument, "HpTF:滤波器行缺少可解析的 Fc", line());
    case 3:
        return make_error(ErrorCode::invalid_argument, "HpTF:Gain 或 Q 数值无法解析", line());
    default:
        return make_error(ErrorCode::invalid_argument, "HpTF:没有可用的 ParametricEQ 参数", {});
    }

    HptfProfile profile;
    profile.preamp_db = result.preamp_db;
    profile.bands.reserve(result.band_count);
    for (std::size_t i = 0; i < result.band_count; ++i) {
        const auto& b = bands[i];
        HptfBand band;
        band.type = static_cast<HptfBandType>(b.kind);
        band.enabled = b.enabled != 0U;
        band.fc_hz = b.frequency;
        band.gain_db = b.gain_db;
        band.q = b.q;
        profile.bands.push_back(band);
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
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    {
        std::vector<float> values{coeffs.preamp_gain, coeffs.max_response_db, coeffs.auto_trim_db, coeffs.preamp_db};
        for (const auto& section :
             std::span{coeffs.sections}.first(std::min<std::size_t>(coeffs.band_count, coeffs.sections.size()))) {
            values.insert(values.end(), {section.b0, section.b1, section.b2, section.a1, section.a2});
        }
        const auto key = "hptf/r" + std::to_string(revision) + ".10-coefficients";
        consistency::dump(key + ".f32", values);
        consistency::dump(key + "-shape.i32",
                          {static_cast<int>(coeffs.sample_rate), static_cast<int>(coeffs.band_count)});
    }
#endif
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
