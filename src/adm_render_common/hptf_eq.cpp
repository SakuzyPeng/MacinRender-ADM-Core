#include "hptf_eq.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <utility>

#include "adm/c_api.h"

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
    return MradmDspHptfSnapshot{
        to_rust(value->coefficients), value->revision, to_rust(value->right), value->independent ? 1U : 0U};
}
HptfSnapshot from_rust(const MradmDspHptfSnapshot& value) noexcept {
    return {from_rust(value.coefficients), value.revision, from_rust(value.right), value.independent != 0U};
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
    std::array<char, 256> validation{};
    std::size_t invalid_band = 0;
    const auto status = mradm_dsp_hptf_validate(
        bands.data(), result.band_count, result.preamp_db, &invalid_band, validation.data(), validation.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status),
                          validation.data(),
                          invalid_band == 0 ? "" : "band=" + std::to_string(invalid_band));
    }
    return profile;
}

Result<HptfSettings> prepare_hptf(const void* config, std::uint32_t source_kind) {
    void* raw = nullptr;
    std::array<char, 512> message{};
    const auto status = mradm_dsp_hptf_settings_create(config, source_kind, &raw, message.data(), message.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), message.data());
    }
    return HptfSettings{std::shared_ptr<void>{raw, mradm_dsp_hptf_settings_destroy}};
}
Result<HptfSettings> prepare_hptf(const HptfProfile& profile, HptfPreampMode mode, std::uint64_t revision) {
    if (profile.bands.size() > std::numeric_limits<uint32_t>::max()) {
        return make_error(ErrorCode::invalid_argument, "HpTF:滤波器数组过大");
    }
    std::vector<adm_hptf_band_t> bands;
    bands.reserve(profile.bands.size());
    std::ranges::transform(profile.bands, std::back_inserter(bands), [](const HptfBand& b) {
        return adm_hptf_band_t{
            sizeof(adm_hptf_band_t), static_cast<int32_t>(b.type), b.enabled ? 1 : 0, 0, b.fc_hz, b.gain_db, b.q};
    });
    const adm_hptf_parameters_t parameters{sizeof(adm_hptf_parameters_t),
                                           static_cast<uint32_t>(bands.size()),
                                           bands.data(),
                                           profile.preamp_db,
                                           static_cast<int32_t>(mode),
                                           0,
                                           revision};
    return prepare_hptf(&parameters, 1);
}
Result<HptfSettings> prepare_hptf(const std::string& path, HptfPreampMode mode, std::uint64_t revision) {
    const adm_hptf_config_t config{sizeof(adm_hptf_config_t), path.c_str(), static_cast<int32_t>(mode), 0, revision};
    return prepare_hptf(&config, 0);
}
Result<HptfSnapshot> HptfSettings::design(std::uint32_t rate) const {
    MradmDspHptfSnapshot result{};
    std::array<char, 512> message{};
    const auto status = mradm_dsp_hptf_settings_design(handle.get(), rate, &result, message.data(), message.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), message.data());
    }
    return from_rust(result);
}
HptfStereoInfo hptf_snapshot_info(const HptfSnapshot& snapshot) {
    const auto info = [&](const HptfCoefficients& c) -> HptfInfo {
        return {!c.is_bypass(),
                c.band_count,
                c.sample_rate,
                c.preamp_db,
                c.auto_trim_db,
                c.max_response_db,
                snapshot.revision};
    };
    const auto& right = snapshot.independent ? snapshot.right : snapshot.coefficients;
    return {info(snapshot.coefficients), info(right), right == snapshot.coefficients};
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
    publish(HptfSnapshot{coeffs, revision});
}
void HptfProcessor::publish(const HptfSnapshot& snapshot) {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    const auto dump = [&](const HptfCoefficients& coeffs, const std::string& side) {
        std::vector<float> values{coeffs.preamp_gain, coeffs.max_response_db, coeffs.auto_trim_db, coeffs.preamp_db};
        for (const auto& section : std::span{coeffs.sections}.first(coeffs.band_count)) {
            values.insert(values.end(), {section.b0, section.b1, section.b2, section.a1, section.a2});
        }
        const auto key = "hptf/r" + std::to_string(snapshot.revision) + ".10-" + side + "coefficients";
        consistency::dump(key + ".f32", values);
        consistency::dump(key + "-shape.i32",
                          {static_cast<int>(coeffs.sample_rate), static_cast<int>(coeffs.band_count)});
    };
    dump(snapshot.coefficients, snapshot.independent ? "left-" : "");
    if (snapshot.independent) {
        dump(snapshot.right, "right-");
    }
#endif
    const std::lock_guard lock(publication_mutex_);
    pending_.publish(snapshot);
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
    // A target published during a fade replaces any target Rust has queued and
    // starts at the frame where that fade ends, independent of call sizes.
    const auto target = to_rust(pending_.consume());
    MradmDspHptfUpdate update{};
    check_realtime(mradm_dsp_hptf_processor_process(state_->handle.get(),
                                                    interleaved,
                                                    audio_length(frames, channels_),
                                                    target ? &*target : nullptr,
                                                    &update,
                                                    nullptr,
                                                    0U));
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
