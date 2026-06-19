#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <ebur128.h>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <saf_utility_complex.h>
#include <saf_utility_fft.h>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <bw64/bw64.hpp>
#include <ear/ear.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_ear.h"

#include "render_common.h"
#include "speaker_layouts.h"

namespace mradm {

namespace {

#ifdef _MSC_VER
#define MRADM_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define MRADM_RESTRICT __restrict__
#else
#define MRADM_RESTRICT
#endif

struct BlockGains {
    std::vector<double> gains;
    std::vector<double> diffuse_gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
    std::optional<uint64_t> interp_length_samples;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::string object_id;          // owning SceneObject::id (empty for HOA-pack tracks); live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
};

[[nodiscard]] std::string normalise_speaker_label_key(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

[[nodiscard]] std::string libear_lfe_label_for_block(const SceneDirectSpeakersBlock& ds) {
    for (const auto& label : ds.speaker_labels) {
        const std::string key = normalise_speaker_label_key(label);
        if (key.find("LFE2") != std::string::npos || key == "LFER") {
            return "LFE2";
        }
    }
    return "LFE1";
}

[[nodiscard]] std::vector<std::string> speaker_labels_for_libear(const SceneDirectSpeakersBlock& ds) {
    if (render_common::direct_speakers_block_is_lfe(ds)) {
        return {libear_lfe_label_for_block(ds)};
    }
    return ds.speaker_labels;
}

// Accumulation uses non-interleaved (channel-major) temporary buffers so the
// innermost loop over frames is a plain saxpy — SIMD-vectorisable by the compiler.
// Layout: col_buf[out_ch * frames_cap + frame], contiguous per output channel.
struct AccumulateContext {
    const float* input{nullptr};
    float* col_direct{nullptr};  // [num_out_ch × frames_cap] column-major, float
    float* col_diffuse{nullptr}; // [num_out_ch × frames_cap] column-major, float
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
    uint64_t object_smoothing_frames{0};
    uint64_t frames_cap{0}; // stride between columns (= k_block_size)
};

// FIR decorrelator state for the diffuse bus (BS.2127).
// Uses overlap-add FFT convolution via saf_rfft (KissFFT backend, platform-agnostic).
// FFT size L=2048 (next power-of-2 >= block_size(1024) + filter_len(512) - 1).
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct DecorrState {
    void* hFFT{nullptr};                               // saf_rfft handle, L=2048
    std::vector<std::vector<float_complex>> filter_fd; // [num_out_ch][L/2+1=1025]
    std::vector<std::vector<float>> overlap;           // [num_out_ch][K-1=511]
    std::size_t fft_len{0};
    std::size_t bins{0};
    std::size_t overlap_len{0};
    int comp_delay{0};                         // decorrelatorCompensationDelay()=255
    std::vector<std::vector<float>> dir_delay; // [num_out_ch][comp_delay]

    ~DecorrState() {
        if (hFFT != nullptr) {
            saf_rfft_destroy(&hFFT);
        }
    }
    DecorrState() = default;
    DecorrState(const DecorrState&) = delete;
    DecorrState& operator=(const DecorrState&) = delete;
    DecorrState(DecorrState&&) = delete;
    DecorrState& operator=(DecorrState&&) = delete;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::vector<SceneOutputSpeaker> output_speakers(const ear::Layout& layout) {
    std::vector<SceneOutputSpeaker> result;
    result.reserve(layout.channels().size());
    for (const auto& channel : layout.channels()) {
        const auto pos = channel.polarPosition();
        result.push_back({static_cast<float>(pos.azimuth), static_cast<float>(pos.elevation), channel.isLfe()});
    }
    return result;
}

[[nodiscard]] boost::optional<std::pair<double, double>>
to_ear_range(const std::optional<std::pair<float, float>>& range) {
    if (!range.has_value()) {
        return boost::none;
    }
    return std::make_pair(static_cast<double>(range->first), static_cast<double>(range->second));
}

[[nodiscard]] ear::Channel make_ear_channel(const render_layouts::SpeakerSpec& speaker) {
    const ear::PolarPosition pos{static_cast<double>(speaker.azimuth), static_cast<double>(speaker.elevation)};
    return {std::string{speaker.label},
            pos,
            pos,
            to_ear_range(speaker.azimuth_range),
            to_ear_range(speaker.elevation_range),
            speaker.is_lfe};
}

[[nodiscard]] ear::Layout make_custom_ear_layout(const render_layouts::SpeakerLayout& spec) {
    std::vector<ear::Channel> channels;
    channels.reserve(spec.speakers.size());
    std::ranges::transform(spec.speakers, std::back_inserter(channels), make_ear_channel);
    return {std::string{spec.id}, std::move(channels)};
}

[[nodiscard]] bool needs_project_ear_layout(std::string_view layout_id) {
    return layout_id == "4+5+4" || layout_id == "9.1.6";
}

[[nodiscard]] ear::Layout make_ear_layout(std::string_view layout_id) {
    if (layout_id == "wav71") {
        return ear::getLayout("0+7+0");
    }
    if (needs_project_ear_layout(layout_id)) {
        if (const auto* layout = render_layouts::find_speaker_layout(layout_id); layout != nullptr) {
            return make_custom_ear_layout(*layout);
        }
    }
    return ear::getLayout(std::string{layout_id});
}

[[nodiscard]] std::size_t next_power_of_two(std::size_t value) {
    std::size_t out = 1;
    while (out < value) {
        out <<= 1U;
    }
    return out;
}

[[nodiscard]] ear::ObjectsTypeMetadata object_metadata_from_block(const SceneObjectBlock& block,
                                                                  const SceneObject& obj) {
    ear::ObjectsTypeMetadata meta;

    const auto pos = scene_position_to_polar(block.position);
    meta.position = ear::PolarPosition{
        static_cast<double>(pos.azimuth),
        static_cast<double>(pos.elevation),
        static_cast<double>(pos.distance),
    };
    meta.cartesian = false;
    meta.gain = static_cast<double>(block.gain) * static_cast<double>(obj.gain);
    meta.diffuse = static_cast<double>(block.diffuse);
    meta.width = static_cast<double>(block.width);
    meta.height = static_cast<double>(block.height);
    meta.depth = static_cast<double>(block.depth);
    return meta;
}

void append_object_blocks(const SceneTrackRef& track,
                          const SceneObject& obj,
                          ChannelGainInfo& cg,
                          ear::GainCalculatorObjects& objects_calc,
                          const std::vector<SceneOutputSpeaker>& speakers,
                          std::size_t num_out,
                          LogSink& logs,
                          bool& screen_ref_warned) {
    for (const auto& raw_block : track.blocks) {
        const auto prepared =
            render_common::prepare_object_block(raw_block, obj, speakers, logs, "ear", screen_ref_warned);
        BlockGains bg;
        bg.gains.resize(num_out, 0.0);
        bg.diffuse_gains.resize(num_out, 0.0);
        bg.start_sample = prepared.start_sample;
        bg.end_sample = prepared.end_sample;
        bg.jump_position = prepared.jump_position;
        bg.smoothable_object = true;
        bg.interp_length_samples = prepared.interp_length_samples;

        for (const auto& source : prepared.sources) {
            auto meta = object_metadata_from_block(source, obj);
            std::vector<double> direct(num_out, 0.0);
            std::vector<double> diffuse(num_out, 0.0);
            // meta.channelLock / objectDivergence / screenRef remain default;
            // project-owned preprocessing above keeps libear away from its
            // not_implemented paths for these fields.
            objects_calc.calculate(meta, direct, diffuse);
            for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
                bg.gains[out_ch] += direct[out_ch];
                bg.diffuse_gains[out_ch] += diffuse[out_ch];
            }
        }
        cg.blocks.push_back(std::move(bg));
    }
}

[[nodiscard]] ear::DirectSpeakersTypeMetadata direct_speakers_metadata_from_block(const SceneDirectSpeakersBlock& ds) {
    ear::DirectSpeakersTypeMetadata meta;
    const bool is_lfe = render_common::direct_speakers_block_is_lfe(ds);
    meta.speakerLabels = speaker_labels_for_libear(ds);
    // libear throws if audioPackFormatID is set without speaker labels (including
    // non-common-definition IDs). Only pass the ID when labels are also present so
    // that label-less custom DS blocks fall through to position-based routing.
    if (!ds.pack_format_id.empty() && !ds.speaker_labels.empty()) {
        meta.audioPackFormatID = ds.pack_format_id;
    }
    if (ds.has_position) {
        ear::PolarSpeakerPosition psp{
            static_cast<double>(ds.azimuth),
            static_cast<double>(ds.elevation),
            static_cast<double>(ds.distance),
        };
        if (ds.azimuth_min) {
            psp.azimuthMin = static_cast<double>(*ds.azimuth_min);
        }
        if (ds.azimuth_max) {
            psp.azimuthMax = static_cast<double>(*ds.azimuth_max);
        }
        if (ds.elevation_min) {
            psp.elevationMin = static_cast<double>(*ds.elevation_min);
        }
        if (ds.elevation_max) {
            psp.elevationMax = static_cast<double>(*ds.elevation_max);
        }
        if (ds.distance_min) {
            psp.distanceMin = static_cast<double>(*ds.distance_min);
        }
        if (ds.distance_max) {
            psp.distanceMax = static_cast<double>(*ds.distance_max);
        }
        meta.position = psp;
    }
    if (ds.low_pass_hz) {
        meta.channelFrequency.lowPass = static_cast<double>(*ds.low_pass_hz);
    } else if (is_lfe) {
        meta.channelFrequency.lowPass = 120.0;
    }
    return meta;
}

void append_direct_speakers_blocks(const SceneTrackRef& track,
                                   const SceneObject& obj,
                                   ChannelGainInfo& cg,
                                   ear::GainCalculatorDirectSpeakers& direct_speakers_calc,
                                   std::size_t num_out) {
    for (const auto& ds : track.ds_blocks) {
        auto meta = direct_speakers_metadata_from_block(ds);
        BlockGains bg;
        bg.gains.resize(num_out, 0.0);
        bg.diffuse_gains.resize(num_out, 0.0); // DS has no diffuse bus
        bg.start_sample = ds.start_sample;
        bg.end_sample = std::min(ds.end_sample, obj.end_sample);
        bg.jump_position = true;
        direct_speakers_calc.calculate(meta, bg.gains);
        const auto ds_gain = static_cast<double>(ds.gain) * static_cast<double>(obj.gain);
        std::ranges::transform(bg.gains, bg.gains.begin(), [ds_gain](double g) { return g * ds_gain; });
        cg.blocks.push_back(std::move(bg));
    }
}

void append_hoa_blocks(const SceneHOATracks& pack,
                       std::map<uint16_t, ChannelGainInfo>& by_channel,
                       ear::GainCalculatorHOA& hoa_calc,
                       std::size_t num_out) {
    const std::size_t n_hoa = pack.channels.size();
    if (n_hoa == 0) {
        return;
    }

    ear::HOATypeMetadata meta;
    meta.normalization = pack.normalization;
    meta.nfcRefDist = pack.nfc_ref_dist;
    meta.screenRef = pack.screen_ref;
    meta.orders.resize(n_hoa);
    meta.degrees.resize(n_hoa);
    for (std::size_t i = 0; i < n_hoa; ++i) {
        meta.orders[i] = pack.channels[i].order;
        meta.degrees[i] = pack.channels[i].degree;
    }

    // decode_matrix[i][out_ch] = gain for HOA channel i → output channel out_ch.
    std::vector<std::vector<double>> decode_matrix(n_hoa, std::vector<double>(num_out, 0.0));
    hoa_calc.calculate(meta, decode_matrix);

    const auto obj_gain = static_cast<double>(pack.gain);

    for (std::size_t i = 0; i < n_hoa; ++i) {
        const auto& ch = pack.channels[i];
        if (!ch.channel_index.has_value()) {
            continue;
        }
        const uint16_t in_ch = *ch.channel_index;
        auto& cg = by_channel[in_ch];
        cg.input_channel = in_ch;

        // One BlockGains per AudioBlockFormatHoa block; the decode matrix row is
        // fixed (order/degree unchanged across blocks), only the gain scalar varies.
        for (const auto& hblk : ch.blocks) {
            const double ch_gain = static_cast<double>(hblk.gain) * obj_gain;
            BlockGains bg;
            bg.gains.resize(num_out, 0.0);
            bg.diffuse_gains.resize(num_out, 0.0);
            bg.start_sample = hblk.start_sample;
            bg.end_sample = hblk.end_sample;
            bg.jump_position = true; // decode matrix is static — no interpolation ramp
            for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
                bg.gains[out_ch] = decode_matrix[i][out_ch] * ch_gain;
            }
            cg.blocks.push_back(std::move(bg));
        }
    }
}

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, const ear::Layout& layout, LogSink& logs) {
    std::map<uint16_t, ChannelGainInfo> by_channel;
    ear::GainCalculatorObjects objects_calc{layout};
    ear::GainCalculatorDirectSpeakers direct_speakers_calc{layout};
    ear::GainCalculatorHOA hoa_calc{layout};
    const std::size_t num_out = layout.channels().size();
    const auto speakers = output_speakers(layout);
    bool screen_ref_warned{false};

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = *track.channel_index;
            auto& cg = by_channel[in_ch];
            cg.input_channel = in_ch;
            cg.object_id = obj.id;
            append_object_blocks(track, obj, cg, objects_calc, speakers, num_out, logs, screen_ref_warned);
            append_direct_speakers_blocks(track, obj, cg, direct_speakers_calc, num_out);
        }
    }

    for (const auto& pack : scene.hoa_tracks) {
        if (!pack.mute) {
            append_hoa_blocks(pack, by_channel, hoa_calc, num_out);
        }
    }

    std::vector<ChannelGainInfo> result;
    result.reserve(by_channel.size());
    for (auto& [ch, cg] : by_channel) {
        std::ranges::sort(cg.blocks, {}, &BlockGains::start_sample);
        result.push_back(std::move(cg));
    }
    return result;
}

void accumulate_channel_segment(const ChannelGainInfo& channel,
                                std::size_t block_index,
                                const AccumulateContext& ctx,
                                const float* ch_in,
                                std::size_t f0,
                                std::size_t f1) {
    const auto& block = channel.blocks[block_index];
    const uint64_t abs_start = ctx.frames_done;

    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta0 = (abs_start + f0) - block.start_sample;
    const bool any_ramp = interp_len > 0 && delta0 < interp_len;

    const std::size_t num_out = ctx.num_out_ch;

    const uint64_t stride = ctx.frames_cap;

    if (!any_ramp) {
        // Fast path: gains constant over this window → saxpy per output channel.
        for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
            const auto gd = static_cast<float>(block.gains[out_ch]);
            const auto gf = static_cast<float>(block.diffuse_gains[out_ch]);
            if (gd == 0.0F && gf == 0.0F) {
                continue; // skip sparse zeros (common for VBAP panning)
            }
            float* MRADM_RESTRICT col_d = ctx.col_direct + (out_ch * stride);
            float* MRADM_RESTRICT col_f = ctx.col_diffuse + (out_ch * stride);
            if (gd != 0.0F) {
                for (std::size_t f = f0; f < f1; ++f) {
                    col_d[f] += ch_in[f] * gd;
                }
            }
            if (gf != 0.0F) {
                for (std::size_t f = f0; f < f1; ++f) {
                    col_f[f] += ch_in[f] * gf;
                }
            }
        }
    } else {
        // Slow path: interpolating — per-frame scalar fallback.
        for (std::size_t f = f0; f < f1; ++f) {
            const uint64_t delta = (abs_start + f) - block.start_sample;
            const bool ramping = delta < interp_len;
            const float in = ch_in[f];
            for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
                const auto gd = static_cast<float>(
                    ramping ? render_common::interpolated_scalar(
                                  channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len)
                            : block.gains[out_ch]);
                const auto gf = static_cast<float>(
                    ramping ? render_common::interpolated_scalar(channel.blocks[block_index - 1].diffuse_gains[out_ch],
                                                                 block.diffuse_gains[out_ch],
                                                                 delta,
                                                                 interp_len)
                            : block.diffuse_gains[out_ch]);
                ctx.col_direct[(out_ch * stride) + f] += in * gd;
                ctx.col_diffuse[(out_ch * stride) + f] += in * gf;
            }
        }
    }
}

// Accumulate one input channel into the column-major direct/diffuse buffers.
// Fast path (static gains): inner loop is a plain saxpy -> auto-vectorised.
// Slow path (ramping): per-frame scalar fallback.
void accumulate_channel_block(const ChannelGainInfo& channel,
                              std::size_t& block_index,
                              const AccumulateContext& ctx,
                              const float* ch_in, // deinterleaved, [frames_now]
                              uint64_t frames_now) {
    const uint64_t abs_start = ctx.frames_done;
    const uint64_t win_end = abs_start + frames_now;
    std::size_t f0 = 0;

    while (f0 < frames_now) {
        const uint64_t abs_frame = abs_start + f0;
        while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
            ++block_index;
        }

        const auto& block = channel.blocks[block_index];
        if (abs_frame < block.start_sample) {
            f0 = static_cast<std::size_t>(std::min(block.start_sample - abs_start, win_end - abs_start));
            continue;
        }
        if (abs_frame >= block.end_sample) {
            if (block_index + 1 >= channel.blocks.size()) {
                return;
            }
            const uint64_t next_start = channel.blocks[block_index + 1].start_sample;
            f0 = static_cast<std::size_t>(std::min(std::max(abs_frame, next_start) - abs_start, frames_now));
            ++block_index;
            continue;
        }

        uint64_t segment_end = std::min(block.end_sample, win_end);
        if (block_index + 1 < channel.blocks.size()) {
            segment_end = std::min(segment_end, channel.blocks[block_index + 1].start_sample);
        }
        if (segment_end <= abs_frame) {
            ++block_index;
            continue;
        }

        const auto f1 = static_cast<std::size_t>(segment_end - abs_start);
        accumulate_channel_segment(channel, block_index, ctx, ch_in, f0, f1);
        f0 = f1;
    }
}

[[nodiscard]] bool gains_at_frame(const ChannelGainInfo& channel,
                                  std::size_t& block_index,
                                  const AccumulateContext& ctx,
                                  uint64_t abs_frame,
                                  std::vector<float>& direct,
                                  std::vector<float>& diffuse) {
    std::ranges::fill(direct, 0.0F);
    std::ranges::fill(diffuse, 0.0F);
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return false;
    }

    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;
    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        direct[out_ch] = static_cast<float>(block.gains[out_ch]);
        diffuse[out_ch] = static_cast<float>(block.diffuse_gains[out_ch]);
        if (ramping) {
            direct[out_ch] = render_common::interpolated_scalar(
                static_cast<float>(channel.blocks[block_index - 1].gains[out_ch]), direct[out_ch], delta, interp_len);
            diffuse[out_ch] = render_common::interpolated_scalar(
                static_cast<float>(channel.blocks[block_index - 1].diffuse_gains[out_ch]),
                diffuse[out_ch],
                delta,
                interp_len);
        }
    }
    return block.smoothable_object;
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now,
                            std::vector<float>& ch_in_buf) {
    std::vector<float> start_direct(ctx.num_out_ch);
    std::vector<float> end_direct(ctx.num_out_ch);
    std::vector<float> start_diffuse(ctx.num_out_ch);
    std::vector<float> end_diffuse(ctx.num_out_ch);
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        // Deinterleave this input channel into a contiguous buffer.
        const uint16_t ic = channel.input_channel;
        const uint16_t num_in = ctx.num_in_ch;
        for (std::size_t f = 0; f < frames_now; ++f) {
            ch_in_buf[f] = ctx.input[(f * num_in) + ic];
        }
        auto start_index = block_indices[ci];
        auto end_index = start_index;
        const bool smooth_start =
            ctx.object_smoothing_frames > 0 &&
            gains_at_frame(channel, start_index, ctx, ctx.frames_done, start_direct, start_diffuse);
        const bool smooth_end =
            ctx.object_smoothing_frames > 0 &&
            gains_at_frame(channel, end_index, ctx, ctx.frames_done + frames_now - 1, end_direct, end_diffuse);
        if (!smooth_start || !smooth_end) {
            accumulate_channel_block(channel, block_indices[ci], ctx, ch_in_buf.data(), frames_now);
            continue;
        }
        block_indices[ci] = start_index;
        const uint64_t stride = ctx.frames_cap;
        for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
            float* MRADM_RESTRICT col_d = ctx.col_direct + (out_ch * stride);
            float* MRADM_RESTRICT col_f = ctx.col_diffuse + (out_ch * stride);
            for (std::size_t f = 0; f < frames_now; ++f) {
                const float alpha = frames_now > 1 ? static_cast<float>(f) / static_cast<float>(frames_now - 1) : 0.0F;
                const float gd = (start_direct[out_ch] * (1.0F - alpha)) + (end_direct[out_ch] * alpha);
                const float gf = (start_diffuse[out_ch] * (1.0F - alpha)) + (end_diffuse[out_ch] * alpha);
                col_d[f] += ch_in_buf[f] * gd;
                col_f[f] += ch_in_buf[f] * gf;
            }
        }
    }
}

// Apply 512-tap FIR decorrelator via overlap-add FFT convolution (saf_rfft / KissFFT).
// diffuse_in:  [frames_now × num_out_ch] interleaved, float
// diffuse_out: [frames_now × num_out_ch] interleaved, float  (written)
void apply_decorrelator(DecorrState& state,
                        const std::vector<float>& diffuse_in,
                        std::vector<float>& diffuse_out,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    // Per-call scratch — small fixed size, stack-friendly via vector.
    std::vector<float> buf(state.fft_len);
    std::vector<float_complex> x_fd(state.bins);
    std::vector<float_complex> y_fd(state.bins);
    std::vector<float> y(state.fft_len);

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        // Deinterleave, zero-pad remainder.
        std::ranges::fill(buf, 0.0F);
        for (std::size_t f = 0; f < frames_now; ++f) {
            buf[f] = diffuse_in[(f * num_out_ch) + ch];
        }

        saf_rfft_forward(state.hFFT, buf.data(), x_fd.data());

        for (std::size_t b = 0; b < state.bins; ++b) {
            y_fd[b] = x_fd[b] * state.filter_fd[ch][b];
        }

        // saf_rfft_backward scales by 1/N internally — no extra scaling needed.
        saf_rfft_backward(state.hFFT, y_fd.data(), y.data());

        // Overlap-add: accumulate saved tail into this block's output.
        auto& ovl = state.overlap[ch];
        for (std::size_t f = 0; f < frames_now; ++f) {
            diffuse_out[(f * num_out_ch) + ch] = y[f] + (f < state.overlap_len ? ovl[f] : 0.0F);
        }

        // Save new tail (y[frames_now .. frames_now + k_overlap_len - 1]).
        for (std::size_t i = 0; i < state.overlap_len; ++i) {
            ovl[i] = y[frames_now + i];
        }
    }
}

// Delay the direct bus by comp_delay samples using a circular history buffer.
// Operates in-place on direct_block [frames_now × num_out_ch].
void apply_direct_delay(DecorrState& state,
                        std::vector<float>& direct_block,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    const auto delay = static_cast<std::size_t>(state.comp_delay); // 255

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        auto& buf = state.dir_delay[ch]; // [delay]

        // Snapshot the new samples before in-place modification.
        std::vector<float> new_in(frames_now);
        for (std::size_t f = 0; f < frames_now; ++f) {
            new_in[f] = direct_block[(f * num_out_ch) + ch];
        }

        // Output: up to D samples from the delay buffer, then new_in offset by D.
        // Works for any frames_now, including short tail blocks < D.
        const std::size_t from_buf = std::min(delay, frames_now);
        for (std::size_t f = 0; f < from_buf; ++f) {
            direct_block[(f * num_out_ch) + ch] = buf[f];
        }
        for (std::size_t f = from_buf; f < frames_now; ++f) {
            direct_block[(f * num_out_ch) + ch] = new_in[f - delay];
        }

        // Update delay buffer: evict consumed samples, append new_in.
        if (frames_now >= delay) {
            std::ranges::copy(new_in.end() - static_cast<std::ptrdiff_t>(delay), new_in.end(), buf.begin());
        } else {
            // Shift remaining delay left by frames_now, then append new_in at end.
            std::ranges::copy(buf.begin() + static_cast<std::ptrdiff_t>(frames_now), buf.end(), buf.begin());
            std::ranges::copy(new_in, buf.end() - static_cast<std::ptrdiff_t>(frames_now));
        }
    }
}

void remap_wav71_to_wave_order(std::vector<float>& block, std::size_t frames_now, std::size_t num_out_ch) {
    if (num_out_ch != 8U) {
        return;
    }
    // libear BS.2051 0+7+0 is L R C LFE Ls Rs Lrs Rrs; WAVE_7_1 is
    // L R C LFE Lrs Rrs Ls Rs.
    for (std::size_t f = 0; f < frames_now; ++f) {
        const auto base = f * num_out_ch;
        std::swap(block[base + 4U], block[base + 6U]);
        std::swap(block[base + 5U], block[base + 7U]);
    }
}

// Render one block [frames_done, frames_done+frames_now): accumulate the gain matrix into
// the column-major direct/diffuse scratch, transpose to interleaved, run the FIR
// decorrelator on the diffuse bus + the compensation delay on the direct bus, mix, and
// (for wav71) remap to WAVE order — writing num_out_ch×frames_now interleaved into
// out_block ([0, out_samples) only; caller sizes it). `decorr` + `blk_idx` carry the
// decorrelator overlap / compensation-delay / block cursors across calls; the scratch
// vectors are caller-owned and reused. Extracted verbatim from render_window's loop so the
// offline batch path and the realtime EarStream share one implementation and cannot drift.
void render_ear_block(const std::vector<ChannelGainInfo>& gain_matrix,
                      std::vector<std::size_t>& blk_idx,
                      DecorrState& decorr,
                      const std::vector<float>& in_block,
                      std::vector<float>& out_block,
                      std::vector<float>& col_direct,
                      std::vector<float>& col_diffuse,
                      std::vector<float>& diffuse_in,
                      std::vector<float>& diffuse_out,
                      std::vector<float>& ch_in_buf,
                      std::size_t col_stride,
                      uint64_t frames_done,
                      uint64_t frames_now,
                      uint16_t num_in_ch,
                      uint16_t num_out_ch,
                      uint64_t default_interp,
                      uint64_t object_smoothing_frames,
                      const std::string& output_layout) {
    const std::size_t out_samples = num_out_ch * static_cast<std::size_t>(frames_now);

    // Zero the live region of the column-major accumulation buffers (per channel).
    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        std::fill_n(col_direct.data() + (ch * col_stride), frames_now, 0.0F);
        std::fill_n(col_diffuse.data() + (ch * col_stride), frames_now, 0.0F);
    }

    AccumulateContext ctx;
    ctx.input = in_block.data();
    ctx.col_direct = col_direct.data();
    ctx.col_diffuse = col_diffuse.data();
    ctx.frames_done = frames_done;
    ctx.num_in_ch = num_in_ch;
    ctx.num_out_ch = num_out_ch;
    ctx.default_interp = default_interp;
    ctx.object_smoothing_frames = object_smoothing_frames;
    ctx.frames_cap = col_stride;
    accumulate_gain_matrix(gain_matrix, blk_idx, ctx, frames_now, ch_in_buf);

    // Transpose column-major → interleaved for the decorrelator and delay.
    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        const float* src_d = col_direct.data() + (ch * col_stride);
        const float* src_f = col_diffuse.data() + (ch * col_stride);
        for (std::size_t f = 0; f < frames_now; ++f) {
            out_block[(f * num_out_ch) + ch] = src_d[f];
            diffuse_in[(f * num_out_ch) + ch] = src_f[f];
        }
    }

    apply_decorrelator(decorr, diffuse_in, diffuse_out, frames_now, num_out_ch);
    apply_direct_delay(decorr, out_block, frames_now, num_out_ch);
    for (std::size_t s = 0; s < out_samples; ++s) {
        out_block[s] += diffuse_out[s];
    }
    if (output_layout == "wav71") {
        remap_wav71_to_wave_order(out_block, frames_now, num_out_ch);
    }
}

// Build the FIR decorrelator state (FFT plan + per-channel frequency-domain filters +
// zeroed overlap / compensation-delay buffers) for a layout. Shared by the offline
// render_window and the realtime EarStream so the two never drift.
void init_decorr_state(DecorrState& decorr, const ear::Layout& layout, uint16_t num_out_ch, uint64_t k_block_size) {
    constexpr std::size_t k_fir_len = 512;
    const std::size_t k_fft_len = next_power_of_two(static_cast<std::size_t>(k_block_size) + k_fir_len - 1U);
    const std::size_t k_bins = (k_fft_len / 2U) + 1U;

    saf_rfft_create(&decorr.hFFT, static_cast<int>(k_fft_len));
    decorr.fft_len = k_fft_len;
    decorr.bins = k_bins;
    decorr.overlap_len = k_fir_len - 1U;
    decorr.comp_delay = ear::decorrelatorCompensationDelay(); // 255
    decorr.overlap.assign(num_out_ch, std::vector<float>(decorr.overlap_len, 0.0F));
    decorr.dir_delay.assign(num_out_ch, std::vector<float>(static_cast<std::size_t>(decorr.comp_delay), 0.0F));

    const auto raw_filters = ear::designDecorrelators<float>(layout);
    decorr.filter_fd.resize(num_out_ch, std::vector<float_complex>(k_bins));
    std::vector<float> fir_buf(k_fft_len, 0.0F);
    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        std::ranges::fill(fir_buf, 0.0F);
        const auto& fir = raw_filters[ch];
        std::ranges::copy(fir, fir_buf.begin());
        saf_rfft_forward(decorr.hFFT, fir_buf.data(), decorr.filter_fd[ch].data());
    }
}

// Immutable, reusable EAR state: the resolved libear layout and the per-object gain
// matrix (the expensive libear GainCalculator work). Reused across render_window()
// calls (PreviewSession scrubbing). The decorrelator FFT state is cheap and per-output,
// so it stays in render_window. ear::Layout never leaves this TU (ADR 0003).
struct EarPrepared final : IPreparedRender {
    ear::Layout layout;
    std::vector<ChannelGainInfo> gain_matrix;
};

// Realtime streaming EAR session over the same prepared libear layout + gain matrix as
// render_window. It renders k_block_size-aligned blocks via the SAME render_ear_block the
// offline path uses (carrying the FIR-decorrelator overlap + the direct compensation delay
// across blocks) into a FIFO that process() serves at any requested frame count —
// bit-identical to render_window for a gap-free run from frame 0. seek() zeroes the
// decorrelator overlap / delay (a small discontinuity, acceptable for monitoring) and
// repositions the reader. set_overrides applies live per-object gain by pre-scaling the
// matching input channels before the (linear) gain mix + decorrelation — equal to scaling
// the object gain. The expensive prepared state is borrowed; the factory keeps it alive.
class EarStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<EarStream>>
    create(const EarPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        auto reader = bw64::readFile(plan.input_path);
        if (!reader) {
            return make_error(
                ErrorCode::io_error, "failed to open input for realtime ear stream", "input=" + plan.input_path);
        }
        (void) logs;
        try {
            return std::unique_ptr<EarStream>{new EarStream(prepared, std::move(reader), plan)};
        } catch (const std::exception& e) {
            return make_error(ErrorCode::render_failed, std::string("ear stream setup failed: ") + e.what(), {});
        }
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (frames_done_ >= total_frames_) {
                    break;
                }
                render_block();
                if (fifo_read_ >= fifo_.size()) {
                    break;
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / num_out_ch_;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * num_out_ch_, out.data() + (produced * num_out_ch_));
            fifo_read_ += take * num_out_ch_;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        for (auto& ov : decorr_.overlap) {
            std::ranges::fill(ov, 0.0F);
        }
        for (auto& dl : decorr_.dir_delay) {
            std::ranges::fill(dl, 0.0F);
        }
        std::ranges::fill(blk_idx_, std::size_t{0});
        frames_done_ = std::min(frame, total_frames_);
        render_common::seek_reader_abs(*reader_, frames_done_);
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        std::unordered_map<std::string, float> live;
        for (const auto& ov : overrides.objects) {
            live[ov.object_id] = std::pow(10.0F, ov.gain_db / 20.0F);
        }
        std::ranges::fill(channel_gain_, 1.0F);
        any_live_gain_ = false;
        // Semantic boundary: the override is projected object → input channel (each channel
        // scaled by its owning object's gain). ADM's gain matrix is one object per input
        // channel, so this is exact today; if independently-overridable objects ever shared
        // one input channel they would scale together (last writer wins) — revisit then.
        for (const auto& cg : prepared_.gain_matrix) {
            if (const auto it = live.find(cg.object_id); it != live.end() && cg.input_channel < channel_gain_.size()) {
                channel_gain_[cg.input_channel] = it->second;
                any_live_gain_ = true;
            }
        }
    }

    [[nodiscard]] uint32_t out_channels() const override { return num_out_ch_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

  private:
    EarStream(const EarPrepared& prepared, std::unique_ptr<bw64::Bw64Reader> reader, const RenderPlan& plan)
        : prepared_(prepared), reader_(std::move(reader)), num_in_ch_(plan.scene.info.num_channels),
          num_out_ch_(static_cast<uint16_t>(prepared.layout.channels().size())),
          sample_rate_(plan.scene.info.sample_rate), total_frames_(plan.scene.info.num_frames),
          output_layout_(plan.output_layout), object_smoothing_frames_(plan.object_smoothing_frames),
          k_block_size_(std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          default_interp_(static_cast<uint64_t>(plan.scene.info.sample_rate) * plan.default_interp_ms / 1000U),
          col_stride_(static_cast<std::size_t>(std::max<uint64_t>(1024U, plan.object_smoothing_frames))),
          blk_idx_(prepared.gain_matrix.size(), 0),
          col_direct_(static_cast<std::size_t>(num_out_ch_) * col_stride_, 0.0F),
          col_diffuse_(static_cast<std::size_t>(num_out_ch_) * col_stride_, 0.0F),
          diffuse_in_(static_cast<std::size_t>(num_out_ch_) * col_stride_, 0.0F),
          diffuse_out_(static_cast<std::size_t>(num_out_ch_) * col_stride_, 0.0F), ch_in_buf_(col_stride_, 0.0F),
          in_block_(static_cast<std::size_t>(plan.scene.info.num_channels) * col_stride_, 0.0F),
          channel_gain_(plan.scene.info.num_channels, 1.0F) {
        init_decorr_state(decorr_, prepared.layout, num_out_ch_, k_block_size_);
    }

    void apply_live_gain(uint64_t frames_now) {
        if (!any_live_gain_) {
            return;
        }
        for (std::size_t f = 0; f < frames_now; ++f) {
            float* frame = in_block_.data() + (f * num_in_ch_);
            for (uint16_t c = 0; c < num_in_ch_; ++c) {
                frame[c] *= channel_gain_[c];
            }
        }
    }

    void render_block() {
        const uint64_t frames_now = std::min<uint64_t>(k_block_size_, total_frames_ - frames_done_);
        reader_->read(in_block_.data(), frames_now);
        apply_live_gain(frames_now);
        fifo_.assign(static_cast<std::size_t>(num_out_ch_) * frames_now, 0.0F);
        fifo_read_ = 0;
        render_ear_block(prepared_.gain_matrix,
                         blk_idx_,
                         decorr_,
                         in_block_,
                         fifo_,
                         col_direct_,
                         col_diffuse_,
                         diffuse_in_,
                         diffuse_out_,
                         ch_in_buf_,
                         col_stride_,
                         frames_done_,
                         frames_now,
                         num_in_ch_,
                         num_out_ch_,
                         default_interp_,
                         object_smoothing_frames_,
                         output_layout_);
        frames_done_ += frames_now;
    }

    const EarPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<bw64::Bw64Reader> reader_;
    uint16_t num_in_ch_;
    uint16_t num_out_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    std::string output_layout_;
    uint64_t object_smoothing_frames_;
    uint64_t k_block_size_;
    uint64_t default_interp_;
    std::size_t col_stride_;
    std::vector<std::size_t> blk_idx_;
    DecorrState decorr_;
    std::vector<float> col_direct_;
    std::vector<float> col_diffuse_;
    std::vector<float> diffuse_in_;
    std::vector<float> diffuse_out_;
    std::vector<float> ch_in_buf_;
    std::vector<float> in_block_;
    std::vector<float> channel_gain_; // per-input-channel live gain multiplier (1.0 = none)
    bool any_live_gain_{false};
    std::vector<float> fifo_;
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class EarRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const EarPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "ear: open_stream received an incompatible prepared state", {});
        }
        auto stream = EarStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport EarRenderer::capabilities() const {
    return ear_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> EarRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    ear::Layout layout = make_ear_layout(plan.output_layout);
    auto gain_matrix = build_gain_matrix(plan.scene, layout, logs);

    if (gain_matrix.empty()) {
        logs.log(LogLevel::warning, "ear", "no renderable tracks found (all muted?), writing silence");
    }

    const auto num_in_ch = plan.scene.info.num_channels;
    const auto invalid_channel =
        std::ranges::find_if(gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix.end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    auto prepared = std::make_shared<EarPrepared>();
    prepared->layout = std::move(layout);
    prepared->gain_matrix = std::move(gain_matrix);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> EarRenderer::render_window(const IPreparedRender& prep,
                                                 const RenderPlan& plan,
                                                 ProgressSink& progress,
                                                 LogSink& logs) { // NOLINT(readability-function-size)
    try {
        const auto* prepared = dynamic_cast<const EarPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "ear: render_window received an incompatible prepared state", {});
        }
        const auto& info = plan.scene.info;
        const ear::Layout& layout = prepared->layout;
        const auto& gain_matrix = prepared->gain_matrix;

        const auto num_out_ch = static_cast<uint16_t>(layout.channels().size());
        const auto num_in_ch = info.num_channels;
        const auto num_frames = info.num_frames;
        const auto sample_rate = info.sample_rate;

        logs.log(
            LogLevel::info,
            "ear",
            fmt::format("rendering {} tracks → {} channels, {} frames", gain_matrix.size(), num_out_ch, num_frames));

        progress.on_progress(
            {RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "rendering audio"});

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);

        // Initialise the FIR decorrelator state for the diffuse bus (shared with EarStream).
        DecorrState decorr;
        init_decorr_state(decorr, layout, num_out_ch, k_block_size);

        // Open file for audio only — ADM metadata comes from plan.scene.
        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        std::vector<std::size_t> blk_idx(gain_matrix.size(), 0);

        // Inline loudness + true-peak measurement (BS.1770-4 / EBU R128).
        struct EburFree {
            void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
        };
        using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
        EburPtr lufs_st{
            ebur128_init(num_out_ch, static_cast<unsigned long>(sample_rate), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};

        const std::size_t col_stride = k_block_size; // frames_cap

        // Column-major accumulation buffers [num_out_ch × k_block_size].
        // col_direct[out_ch * col_stride + frame], col_diffuse likewise.
        std::vector<float> col_direct(num_out_ch * col_stride, 0.0F);
        std::vector<float> col_diffuse(num_out_ch * col_stride, 0.0F);

        // Interleaved buffers used for I/O and the decorrelator.
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> diffuse_in(num_out_ch * k_block_size); // interleaved diffuse
        std::vector<float> diffuse_out(num_out_ch * k_block_size);
        std::vector<float> ch_in_buf(k_block_size); // deinterleaved input scratch

        // Loudness / true-peak measurement runs on a background thread (SerialWorker) so it overlaps
        // the next block's mix + decorrelation. Double-buffer the interleaved output so the next block
        // can be produced while the meter still reads the previous one; reuse waits on the outstanding
        // measurement future. FIFO ordering keeps the measured loudness / TP bit-identical.
        constexpr std::size_t k_num_buffers = 2;
        std::array<std::vector<float>, k_num_buffers> out_buffers; // interleaved direct
        for (auto& buffer : out_buffers) {
            buffer.assign(num_out_ch * k_block_size, 0.0F);
        }
        std::array<std::future<void>, k_num_buffers> meter_pending;
        render_common::SerialWorker meter;
        std::size_t buf_idx = 0;

        // Output sub-window with warm-up pre-roll (RenderPlan::render_window). Blocks
        // are processed on the SAME k_block_size grid as a full render so the
        // decorrelator FFT segmentation and direct delay stay bit-identical; one
        // block-aligned pre-roll block ahead of the window converges the decorrelator
        // overlap (k_fir_len-1) and compensation delay. Gains are closed-form per
        // absolute frame, so seeking needs no gain warm-up. When not windowed,
        // win_start=0 / win_end=num_frames reproduces the full-timeline render exactly.
        const bool windowed = plan.render_window.has_value();
        const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
        const uint64_t win_end =
            windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
        uint64_t start_pos = 0;
        if (windowed && win_start >= k_block_size) {
            start_pos = ((win_start / k_block_size) - 1) * k_block_size; // one full block of pre-roll
        }
        if (start_pos > 0) {
            render_common::seek_reader_abs(*reader, start_pos);
        }
        const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
        const auto progress_span = static_cast<double>(progress_total);
        uint64_t frames_done = start_pos;

        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);

            // Sub-range of this block that lies inside the output window [win_start, win_end).
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

            // Reclaim this output buffer once the meter has finished its previous use of it.
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            std::vector<float>& out_block = out_buffers.at(buf_idx);

            reader->read(in_block.data(), frames_now);

            render_ear_block(gain_matrix,
                             blk_idx,
                             decorr,
                             in_block,
                             out_block,
                             col_direct,
                             col_diffuse,
                             diffuse_in,
                             diffuse_out,
                             ch_in_buf,
                             col_stride,
                             frames_done,
                             frames_now,
                             num_in_ch,
                             num_out_ch,
                             k_default_interp,
                             plan.object_smoothing_frames,
                             plan.output_layout);

            // Write only the in-window frames. Pre-roll blocks (emit == false) are
            // processed for state warm-up but not written.
            if (emit && writer.write(out_block.data() + (emit_off * num_out_ch), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }

            // Offload loudness / true-peak measurement to the background meter (overlaps next block).
            // Windowed: meter exactly the written frames. Otherwise: honor meter_window
            // (no-trim → whole block; trim fallback → kept part).
            if (lufs_st) {
                std::size_t meter_off = 0;
                std::size_t meter_count = 0;
                if (windowed) {
                    meter_off = emit_off;
                    meter_count = emit_count;
                } else {
                    const auto chunk = render_common::meter_window_chunk(plan.meter_window, frames_done, frames_now);
                    meter_off = chunk.offset_frames;
                    meter_count = static_cast<std::size_t>(chunk.frame_count);
                }
                if (meter_count > 0) {
                    ebur128_state* state = lufs_st.get();
                    const float* data = out_block.data() + (meter_off * num_out_ch);
                    const auto frame_count = meter_count;
                    meter_pending.at(buf_idx) =
                        meter.post([state, data, frame_count] { ebur128_add_frames_float(state, data, frame_count); });
                }
            }

            frames_done += frames_now;
            buf_idx = (buf_idx + 1) % k_num_buffers;

            const uint64_t progress_done = std::min(frames_done, win_end) - start_pos;
            const double stage_fraction = static_cast<double>(progress_done) / progress_span;
            const double frac = 0.3 + (0.6 * stage_fraction);
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  frac,
                                  stage_fraction,
                                  progress_done,
                                  progress_total,
                                  "rendering"});
        }

        // All audio is written; wait for outstanding measurements before reading global metrics.
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        logs.log(LogLevel::info,
                 "ear",
                 fmt::format("wrote {} frames to {}{}",
                             win_end - win_start,
                             plan.output_path,
                             windowed ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, num_frames)
                                      : std::string{}));

        RenderMetrics metrics;
        if (lufs_st) {
            double loudness = 0.0;
            if (ebur128_loudness_global(lufs_st.get(), &loudness) == EBUR128_SUCCESS && std::isfinite(loudness)) {
                metrics.measured_lufs = loudness;
            }
            double max_peak = 0.0;
            for (unsigned int ch = 0; ch < num_out_ch; ++ch) {
                double ch_peak = 0.0;
                if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, ch_peak);
                }
            }
            if (max_peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
            }
        }
        return metrics;

    } catch (const std::invalid_argument& e) {
        // libear throws std::invalid_argument for unknown layout names
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported output layout '{}': {}", plan.output_layout, e.what()),
                          "layout=" + plan.output_layout);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("render failed: ") + e.what(), "input=" + plan.input_path);
    }
}

} // namespace

#undef MRADM_RESTRICT

CapabilityReport ear_capabilities() {
    CapabilityReport r;
    r.backend_name = "libear";
    r.backend_version = "0.9.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = true; // HOA block decode via GainCalculatorHOA
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_screen_ref = false;
    r.supports_diffuse = true;
    r.supports_render_window = true; // seek + 1-block pre-roll; bit-exact windowed output
    for (const auto& spec : render_layouts::speaker_layouts()) {
        CapabilityReport::Layout layout;
        layout.id = std::string{spec.id};
        layout.display_name = std::string{spec.display_name};
        layout.channel_count = static_cast<uint16_t>(spec.speakers.size());
        layout.lfe_count =
            static_cast<uint16_t>(std::ranges::count_if(spec.speakers, [](const auto& s) { return s.is_lfe; }));
        layout.is_3d = std::ranges::any_of(spec.speakers,
                                           [](const auto& s) { return !s.is_lfe && std::fabs(s.elevation) > 1.0e-6F; });
        layout.supports_spread = true;
        r.supported_layouts.push_back(std::move(layout));
    }
    return r;
}

std::unique_ptr<IRenderer> create_ear_renderer() {
    return std::make_unique<EarRenderer>();
}

} // namespace mradm
