#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ebur128.h>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>
// clang-format off
// saf_utility_complex.h must precede saf_hoa.h: saf_hoa.h opens extern "C" and
// re-includes saf_utility_complex.h inside that block, causing std::complex<float>
// template instantiation in C linkage if the include guard hasn't fired yet.
#include <saf_utility_complex.h>
#include <saf_hoa.h>
// clang-format on

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_hoa.h"

namespace mradm {

namespace {

constexpr std::size_t k_hoa3_channels = 16; // (3+1)^2 = 4^2

constexpr uint16_t k_hoa3_channels_u16 = static_cast<uint16_t>(k_hoa3_channels);
using Hoa3Coeffs = std::array<float, k_hoa3_channels>;

// Compute real SN3D spherical harmonic coefficients for order 3 (ACN channel
// ordering), given a Cartesian unit direction in HOA convention (X=front,
// Y=left, Z=up).
//
// Matches ADMHOAEncoder::encodeOrder3SN3DForDirectionX:y:z: from the ObjC
// renderer. Callers must ensure the vector is already normalised.
Hoa3Coeffs sh_sn3d_3(float x, float y, float z) noexcept {
    constexpr float sqrt3 = std::numbers::sqrt3_v<float>;
    const float sqrt15 = std::sqrt(15.0F);
    const float sqrt5_8 = std::sqrt(5.0F / 8.0F);
    const float sqrt3_8 = std::sqrt(3.0F / 8.0F);

    return {
        // n=0
        1.0F,
        // n=1
        y,
        z,
        x,
        // n=2
        sqrt3 * x * y,
        sqrt3 * y * z,
        0.5F * (3.0F * z * z - 1.0F),
        sqrt3 * x * z,
        0.5F * sqrt3 * (x * x - y * y),
        // n=3
        sqrt5_8 * y * (3.0F * x * x - y * y),
        sqrt15 * x * y * z,
        sqrt3_8 * y * (5.0F * z * z - 1.0F),
        0.5F * z * (5.0F * z * z - 3.0F),
        sqrt3_8 * x * (5.0F * z * z - 1.0F),
        0.5F * sqrt15 * z * (x * x - y * y),
        sqrt5_8 * x * (x * x - 3.0F * y * y),
    };
}

// ADM polar (standard convention: az=0→front, +az→left CCW) to HOA Cartesian
// (X=front, Y=left, Z=up), then encode.
Hoa3Coeffs encode_polar(float az_deg, float el_deg) noexcept {
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const float az = az_deg * k_deg2rad;
    const float el = el_deg * k_deg2rad;
    const float cos_el = std::cos(el);
    // Standard ADM: +az = left (CCW) → sin(az) gives positive Y for left sources.
    return sh_sn3d_3(cos_el * std::cos(az), cos_el * std::sin(az), std::sin(el));
}

// ADM Cartesian (X=right, Y=front, Z=up) → HOA (X=front, Y=left, Z=up).
Hoa3Coeffs encode_cartesian(float xc, float yc, float zc) noexcept {
    const float len = std::max(1.0e-6F, std::hypot(xc, yc, zc));
    return sh_sn3d_3(yc / len, -xc / len, zc / len);
}

// Return true when any label in the list identifies an LFE channel.
// Canonicalises by stripping non-alphanumeric characters and uppercasing so
// that "RC_LFE", "RCLFE", "LFE", "LFE1", "LFE2", "LFEL", "LFER" all match.
// This runs before the position check so that LFE channels with an explicit
// position (e.g. RC_LFE at az=45, el=-30) are still encoded omnidirectionally.
bool is_lfe_label(std::string_view raw) noexcept {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key == "LFE" || key == "LFE1" || key == "LFE2" || key == "LFEL" || key == "LFER" || key == "RCLFE";
}

bool any_label_is_lfe(const std::vector<std::string>& labels) noexcept {
    return std::ranges::any_of(labels, [](const auto& l) { return is_lfe_label(l); });
}

// Parse a BS.2051 speaker label (e.g. "M+030", "U-045", "T+000") into (az, el) degrees.
// Returns nullopt when the label is not a recognised positional format or contains
// trailing non-numeric characters after the azimuth digits.
std::optional<std::pair<float, float>> parse_speaker_label(const std::string& label) {
    if (label.size() < 5) {
        return std::nullopt;
    }
    float el = 0.0F;
    switch (label[0]) {
    case 'M':
        el = 0.0F;
        break;
    case 'U':
        el = 30.0F;
        break;
    case 'T':
        el = 90.0F;
        break;
    case 'B':
        el = -30.0F;
        break;
    default:
        return std::nullopt;
    }
    if (label[1] != '+' && label[1] != '-') {
        return std::nullopt;
    }
    const float sign = (label[1] == '+') ? 1.0F : -1.0F;
    try {
        std::size_t consumed = 0;
        const float az = std::stof(label.substr(2), &consumed) * sign;
        if (2U + consumed != label.size()) {
            return std::nullopt; // trailing garbage (e.g. "M+030foo")
        }
        return std::make_pair(az, el);
    } catch (...) {
        return std::nullopt;
    }
}

struct HoaBlock {
    Hoa3Coeffs gains{};
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool is_lfe{false};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

enum class BlockFilter : uint8_t { all, lfe_only };

[[nodiscard]] bool block_matches_filter(const HoaBlock& block, BlockFilter filter) noexcept {
    return filter == BlockFilter::all || block.is_lfe;
}

struct ChannelGainInfo {
    uint16_t input_channel{0};
    bool has_lfe_block{false};    // true when any ds_block is LFE; used for separate TP tracking
    std::vector<HoaBlock> blocks; // sorted ascending by start_sample
};

// Returns linearly interpolated HOA gains at abs_frame for the given channel.
// Returns all-zero coefficients when abs_frame is outside every block.
[[nodiscard]] Hoa3Coeffs gains_at(const ChannelGainInfo& cg,
                                  uint64_t abs_frame,
                                  uint64_t default_interp,
                                  BlockFilter filter = BlockFilter::all) {
    // Upper-bound search: find the first block whose start_sample > abs_frame.
    const auto it = std::ranges::upper_bound(cg.blocks, abs_frame, {}, &HoaBlock::start_sample);
    if (it == cg.blocks.begin()) {
        return {};
    }
    const auto cur_it = std::prev(it); // cur_it->start_sample <= abs_frame
    const HoaBlock& cur = *cur_it;
    if (abs_frame >= cur.end_sample) {
        return {}; // frame is past this block's end
    }
    if (!block_matches_filter(cur, filter)) {
        return {};
    }
    // Interpolation ramp: blend from previous block gains when jump_position is false.
    if (!cur.jump_position && cur_it != cg.blocks.begin()) {
        const HoaBlock& prev = *std::prev(cur_it);
        if (!block_matches_filter(prev, filter)) {
            return cur.gains;
        }
        // Clamp interp_len to active block duration (mirrors EAR/VBAP interpolation_length()).
        uint64_t active_end = cur.end_sample;
        const auto next_it = std::next(cur_it);
        if (next_it != cg.blocks.end()) {
            active_end = std::min(active_end, next_it->start_sample);
        }
        const uint64_t active_len = (active_end > cur.start_sample) ? (active_end - cur.start_sample) : 0;
        const uint64_t interp_len = std::min(cur.interp_length_samples.value_or(default_interp), active_len);
        const uint64_t delta = abs_frame - cur.start_sample;
        if (interp_len > 0 && delta < interp_len) {
            const double alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
            Hoa3Coeffs result;
            for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
                result.at(i) = static_cast<float>((static_cast<double>(prev.gains.at(i)) * (1.0 - alpha)) +
                                                  (static_cast<double>(cur.gains.at(i)) * alpha));
            }
            return result;
        }
    }
    return cur.gains;
}

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, LogSink& logs) {
    std::map<uint16_t, ChannelGainInfo> by_channel;

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = track.channel_index.value();
            auto& cg = by_channel[in_ch];
            cg.input_channel = in_ch;

            for (const auto& block : track.blocks) {
                const auto& off = obj.position_offset;
                const SceneBlockPosition pos = off ? apply_position_offset(block.position, *off) : block.position;
                Hoa3Coeffs sh =
                    pos.cartesian ? encode_cartesian(pos.x, pos.y, pos.z) : encode_polar(pos.azimuth, pos.elevation);
                const float combined_gain = block.gain * obj.gain;
                std::ranges::transform(sh, sh.begin(), [combined_gain](float c) { return c * combined_gain; });
                cg.blocks.push_back({sh,
                                     block.start_sample,
                                     std::min(block.end_sample, obj.end_sample),
                                     false,
                                     block.jump_position,
                                     block.interp_length_samples});
            }

            for (const auto& ds : track.ds_blocks) {
                Hoa3Coeffs sh{};
                bool is_lfe = false;
                if (ds.low_pass_hz.has_value() || any_label_is_lfe(ds.speaker_labels)) {
                    // LFE: no directionality → encode as omnidirectional (W channel only, ACN 0).
                    // Label check runs before position so channels like RC_LFE (no lowPass element
                    // but carrying a nominal position) are still treated as non-directional.
                    sh[0] = 1.0F;
                    is_lfe = true;
                    cg.has_lfe_block = true;
                } else if (ds.has_position) {
                    sh = encode_polar(ds.azimuth, ds.elevation);
                } else {
                    bool found_label_pos = false;
                    float parsed_azimuth = 0.F;
                    float parsed_elevation = 0.F;
                    for (const auto& label : ds.speaker_labels) {
                        const auto parsed_pos = parse_speaker_label(label);
                        if (parsed_pos.has_value()) {
                            parsed_azimuth = parsed_pos->first;
                            parsed_elevation = parsed_pos->second;
                            found_label_pos = true;
                            break;
                        }
                    }
                    if (!found_label_pos) {
                        logs.log(LogLevel::warning,
                                 "hoa-encode",
                                 fmt::format("DirectSpeakers channel {} has no position and no parseable label; "
                                             "skipping",
                                             in_ch));
                        continue;
                    }
                    sh = encode_polar(parsed_azimuth, parsed_elevation);
                }
                const float combined_gain = ds.gain * obj.gain;
                std::ranges::transform(sh, sh.begin(), [combined_gain](float c) { return c * combined_gain; });
                // DirectSpeakers positions are static — no interpolation needed between blocks.
                cg.blocks.push_back(
                    {sh, ds.start_sample, std::min(ds.end_sample, obj.end_sample), is_lfe, true, std::nullopt});
            }
        }
    }

    std::vector<ChannelGainInfo> result;
    result.reserve(by_channel.size());
    for (auto& [ch, cg] : by_channel) {
        std::ranges::sort(cg.blocks, {}, &HoaBlock::start_sample);
        result.push_back(std::move(cg));
    }
    return result;
}

class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<RenderMetrics> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport HoaRenderer::capabilities() const {
    return hoa_capabilities();
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> HoaRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    if (plan.output_layout != "hoa3") {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported HOA output layout '{}'; supported: hoa3", plan.output_layout),
                          {});
    }

    const auto& info = plan.scene.info;

    auto gain_matrix = build_gain_matrix(plan.scene, logs);
    if (gain_matrix.empty()) {
        logs.log(LogLevel::warning, "hoa-encode", "no renderable tracks found (all muted?), writing silence");
    }

    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
    constexpr uint16_t k_num_out = k_hoa3_channels_u16;

    const auto invalid_channel =
        std::ranges::find_if(gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix.end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    // Build 7.1.4 AllRAD decode matrix for BS.1770 playback-domain measurement.
    // LFE is NOT a spatial speaker — it is excluded from the AllRAD matrix and its
    // decoded slot is zeroed.  ebur128 is initialised for all 12 channels with explicit
    // channel-type assignments so that BS.1770 weighting is applied correctly.
    //
    // SAF getLoudspeakerDecoderMtx uses N3D internally; our HOA output is SN3D.
    // Each column of the decode matrix is scaled by sqrt(2n+1) to compensate.
    constexpr int k_714_nls = 11; // non-LFE spatial speakers for AllRAD
    constexpr int k_714_ch = 12;  // ebur128 channel count (11 spatial + LFE slot)
    constexpr auto k_714_nls_sz = static_cast<std::size_t>(k_714_nls);
    constexpr auto k_714_ch_sz = static_cast<std::size_t>(k_714_ch);
    // clang-format off
    // 11 non-LFE speakers; LFE (az=45, el=-30) deliberately omitted.
    constexpr std::array<float, k_714_nls_sz * 2> k_714_dirs = {
         30.F,   0.F,   // row 0 → ch0  L
        -30.F,   0.F,   // row 1 → ch1  R
          0.F,   0.F,   // row 2 → ch2  C
         90.F,   0.F,   // row 3 → ch4  Ls   (ch3 = LFE slot, zeroed)
        -90.F,   0.F,   // row 4 → ch5  Rs
        135.F,   0.F,   // row 5 → ch6  Lss
       -135.F,   0.F,   // row 6 → ch7  Rss
         45.F,  30.F,   // row 7 → ch8  Ltf
        -45.F,  30.F,   // row 8 → ch9  Rtf
        135.F,  30.F,   // row 9 → ch10 Ltr
       -135.F,  30.F,   // row10 → ch11 Rtr
    };
    constexpr float k_sqrt5 = 2.2360679774997896F;
    constexpr float k_sqrt7 = 2.6457513110645905F;
    constexpr std::array<float, k_hoa3_channels> k_sn3d_to_n3d = {
        1.F,                                                                                     // n=0 (√1)
        std::numbers::sqrt3_v<float>, std::numbers::sqrt3_v<float>, std::numbers::sqrt3_v<float>, // n=1 (√3)
        k_sqrt5, k_sqrt5, k_sqrt5, k_sqrt5, k_sqrt5,                                            // n=2 (√5)
        k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7,                          // n=3 (√7)
    };
    // clang-format on
    std::array<float, k_714_nls_sz * k_hoa3_channels> dec_mtx{};
    {
        // getLoudspeakerDecoderMtx takes a non-const pointer; copy to a local mutable array.
        std::array<float, k_714_nls_sz * 2> dirs_buf = k_714_dirs;
        getLoudspeakerDecoderMtx(dirs_buf.data(), k_714_nls, LOUDSPEAKER_DECODER_ALLRAD, 3, 1, dec_mtx.data());
        for (int ls = 0; ls < k_714_nls; ++ls) {
            for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                dec_mtx.at((static_cast<std::size_t>(ls) * k_hoa3_channels) + sh) *= k_sn3d_to_n3d.at(sh);
            }
        }
    }

    try {
        logs.log(LogLevel::info,
                 "hoa-encode",
                 fmt::format("encoding {} input channels (Objects + DirectSpeakers) → HOA3 ({} ch), {} frames",
                             gain_matrix.size(),
                             k_num_out,
                             num_frames));
        progress.on_progress({RenderStage::rendering, 0.3, "encoding HOA"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, k_num_out, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(k_num_out) * k_block_size);
        std::vector<float> measure_hoa_block(static_cast<std::size_t>(k_num_out) * k_block_size);
        std::vector<float> decoded_block(k_714_ch_sz * k_block_size);

        struct EburFree {
            void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
        };
        using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
        EburPtr lufs_st{ebur128_init(static_cast<unsigned int>(k_714_ch),
                                     static_cast<unsigned long>(sample_rate),
                                     EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};
        if (lufs_st) {
            // Explicit 7.1.4 channel map so BS.1770 weighting is applied correctly.
            // Channels 6+ default to UNUSED in libebur128; set all 12 explicitly.
            ebur128_set_channel(lufs_st.get(), 0U, EBUR128_Mp030);  // L
            ebur128_set_channel(lufs_st.get(), 1U, EBUR128_Mm030);  // R
            ebur128_set_channel(lufs_st.get(), 2U, EBUR128_Mp000);  // C
            ebur128_set_channel(lufs_st.get(), 3U, EBUR128_UNUSED); // LFE (zeroed, excluded)
            ebur128_set_channel(lufs_st.get(), 4U, EBUR128_Mp090);  // Ls
            ebur128_set_channel(lufs_st.get(), 5U, EBUR128_Mm090);  // Rs
            ebur128_set_channel(lufs_st.get(), 6U, EBUR128_Mp135);  // Lss
            ebur128_set_channel(lufs_st.get(), 7U, EBUR128_Mm135);  // Rss
            ebur128_set_channel(lufs_st.get(), 8U, EBUR128_Up045);  // Ltf
            ebur128_set_channel(lufs_st.get(), 9U, EBUR128_Um045);  // Rtf
            ebur128_set_channel(lufs_st.get(), 10U, EBUR128_Up135); // Ltr
            ebur128_set_channel(lufs_st.get(), 11U, EBUR128_Um135); // Rtr
        }

        // Separate TP tracker for LFE channels. LFE is still encoded W-only in the HOA
        // output, but it is subtracted from the HOA measurement buffer before the 7.1.4
        // decode so it cannot contribute to LUFS/spatial TP. Its peak is measured on this
        // mono TP-only state and merged with the spatial decode peak at the end.
        EburPtr lfe_tp_st;
        std::vector<float> lfe_mix_block;
        const bool has_lfe = std::ranges::any_of(gain_matrix, [](const auto& cg) { return cg.has_lfe_block; });
        if (has_lfe) {
            lfe_tp_st.reset(ebur128_init(1U, static_cast<unsigned long>(sample_rate), EBUR128_MODE_TRUE_PEAK));
            lfe_mix_block.resize(k_block_size, 0.F);
        }

        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(k_num_out) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            for (const auto& cg : gain_matrix) {
                if (plan.object_smoothing_frames > 0) {
                    const Hoa3Coeffs start_gains = gains_at(cg, frames_done, k_default_interp);
                    const Hoa3Coeffs end_gains = gains_at(cg, frames_done + frames_now - 1, k_default_interp);
                    for (std::size_t f = 0; f < frames_now; ++f) {
                        const float alpha =
                            frames_now > 1 ? static_cast<float>(f) / static_cast<float>(frames_now - 1) : 0.0F;
                        const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                        std::size_t out_index = f * k_num_out;
                        for (std::size_t out_ch = 0; out_ch < k_num_out; ++out_ch) {
                            const float gain =
                                (start_gains.at(out_ch) * (1.0F - alpha)) + (end_gains.at(out_ch) * alpha);
                            out_block[out_index] += in_s * gain;
                            ++out_index;
                        }
                    }
                    continue;
                }
                for (std::size_t f = 0; f < frames_now; ++f) {
                    const uint64_t abs_frame = frames_done + f;
                    const Hoa3Coeffs gains = gains_at(cg, abs_frame, k_default_interp);
                    const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                    std::size_t out_index = f * k_num_out;
                    for (const float gain : gains) {
                        out_block[out_index] += in_s * gain;
                        ++out_index;
                    }
                }
            }

            if (lufs_st) {
                std::copy_n(out_block.begin(), static_cast<std::ptrdiff_t>(out_samples), measure_hoa_block.begin());
            }

            if (lfe_tp_st) {
                // Mix all LFE input channels into a mono buffer and remove the same W-only
                // contribution from the HOA measurement buffer. gains_at(..., lfe_only)[0]
                // is the combined_gain for LFE blocks; DirectSpeakers have jump_position=true
                // so no interpolation occurs.
                std::fill(lfe_mix_block.begin(), lfe_mix_block.begin() + static_cast<std::ptrdiff_t>(frames_now), 0.F);
                for (const auto& cg : gain_matrix) {
                    if (!cg.has_lfe_block) {
                        continue;
                    }
                    for (std::size_t f = 0; f < frames_now; ++f) {
                        const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                        const Hoa3Coeffs lfe_gains =
                            gains_at(cg, frames_done + f, k_default_interp, BlockFilter::lfe_only);
                        lfe_mix_block[f] += in_s * lfe_gains[0];
                        if (lufs_st) {
                            float* measure_hoa = measure_hoa_block.data() + (f * k_hoa3_channels);
                            for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                                measure_hoa[sh] -= in_s * lfe_gains.at(sh);
                            }
                        }
                    }
                }
                ebur128_add_frames_float(lfe_tp_st.get(), lfe_mix_block.data(), static_cast<std::size_t>(frames_now));
            }

            if (lufs_st) {
                // Decode 16ch HOA → 12ch 7.1.4 for BS.1770 playback-domain measurement.
                // rows 0-2  → ch 0-2 (L R C); ch3 (LFE) = 0; rows 3-10 → ch 4-11.
                for (std::size_t f = 0; f < frames_now; ++f) {
                    const float* hoa = measure_hoa_block.data() + (f * k_hoa3_channels);
                    float* dec = decoded_block.data() + (f * k_714_ch_sz);
                    for (int ls = 0; ls < 3; ++ls) {
                        float s = 0.F;
                        const float* row = dec_mtx.data() + (static_cast<std::size_t>(ls) * k_hoa3_channels);
                        for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                            s += row[sh] * hoa[sh];
                        }
                        dec[ls] = s;
                    }
                    dec[3] = 0.F; // LFE not decoded
                    for (int ls = 3; ls < k_714_nls; ++ls) {
                        float s = 0.F;
                        const float* row = dec_mtx.data() + (static_cast<std::size_t>(ls) * k_hoa3_channels);
                        for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                            s += row[sh] * hoa[sh];
                        }
                        dec[ls + 1] = s; // +1 to skip LFE slot at ch3
                    }
                }
                ebur128_add_frames_float(lufs_st.get(), decoded_block.data(), static_cast<std::size_t>(frames_now));
            }

            if (writer.write(out_block.data(), frames_now) != frames_now) {
                return make_error(ErrorCode::io_error, "short write while encoding HOA", "output=" + plan.output_path);
            }
            frames_done += frames_now;

            const double frac = 0.3 + (0.6 * (static_cast<double>(frames_done) / static_cast<double>(num_frames)));
            progress.on_progress({RenderStage::rendering, frac, "encoding"});
        }

        progress.on_progress({RenderStage::finished, 1.0, "done"});
        logs.log(LogLevel::info, "hoa-encode", fmt::format("wrote {} frames to {}", num_frames, plan.output_path));

        RenderMetrics metrics;
        if (lufs_st) {
            double loudness = 0.0;
            if (ebur128_loudness_global(lufs_st.get(), &loudness) == EBUR128_SUCCESS && std::isfinite(loudness)) {
                metrics.measured_lufs = loudness;
            }
            double max_peak = 0.0;
            for (unsigned int ch = 0; ch < static_cast<unsigned int>(k_714_ch); ++ch) {
                double ch_peak = 0.0;
                if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, ch_peak);
                }
            }
            if (lfe_tp_st) {
                double lfe_peak = 0.0;
                if (ebur128_true_peak(lfe_tp_st.get(), 0U, &lfe_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, lfe_peak);
                }
            }
            if (max_peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
            }
        }
        return metrics;

    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, std::string("HOA encode failed: ") + e.what(), "input=" + plan.input_path);
    }
}

} // namespace

CapabilityReport hoa_capabilities() {
    CapabilityReport r;
    r.backend_name = "hoa-encode";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supported_layouts = {
        {"hoa3", "HOA 3rd Order (16ch, ACN/SN3D)", 16, true, 0, false},
    };
    return r;
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
