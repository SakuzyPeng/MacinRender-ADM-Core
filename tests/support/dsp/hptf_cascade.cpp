#include "hptf_cascade.h"

#include <array>
#include <exception>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "dsp.h"

namespace mradm::render_common {
namespace {

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

} // namespace mradm::render_common
