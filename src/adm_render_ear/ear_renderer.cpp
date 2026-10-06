#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ear/ear.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_ear.h"

#include "consistency_trace.h"
#include "ear_post.h"
#include "meter.h"
#include "render_common.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

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
    std::string speaker_label_key;  // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
};

[[nodiscard]] std::string libear_lfe_label_for_block(const SceneDirectSpeakersBlock& ds) {
    for (const auto& label : ds.speaker_labels) {
        const std::string key = render_common::normalise_speaker_label_key(label);
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

[[nodiscard]] std::vector<SceneOutputSpeaker> output_speakers(const ear::Layout& layout) {
    std::vector<SceneOutputSpeaker> result;
    result.reserve(layout.channels().size());
    for (const auto& channel : layout.channels()) {
        const auto pos = channel.polarPosition();
        result.push_back({static_cast<float>(pos.azimuth), static_cast<float>(pos.elevation), channel.isLfe()});
    }
    return result;
}

[[nodiscard]] std::vector<render_common::DirectSpeakerRoutingTarget> direct_speaker_targets(const ear::Layout& layout) {
    std::vector<render_common::DirectSpeakerRoutingTarget> result;
    result.reserve(layout.channels().size());
    for (const auto& channel : layout.channels()) {
        const auto position = channel.polarPosition();
        result.push_back({channel.name(),
                          static_cast<float>(position.azimuth),
                          static_cast<float>(position.elevation),
                          channel.isLfe()});
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

[[nodiscard]] ear::Layout apply_effective_speaker_positions(ear::Layout layout,
                                                            const render_layouts::SpeakerLayout& effective) {
    for (auto& channel : layout.channels()) {
        const auto speaker = std::ranges::find(effective.speakers, channel.name(), &render_layouts::SpeakerSpec::label);
        if (speaker == effective.speakers.end()) {
            throw std::invalid_argument(
                fmt::format("speaker '{}' is missing from geometry profile '{}'", channel.name(), effective.id));
        }
        channel.polarPosition(
            ear::PolarPosition{static_cast<double>(speaker->azimuth), static_cast<double>(speaker->elevation)});
        channel.azimuthRange(to_ear_range(speaker->azimuth_range));
        channel.elevationRange(to_ear_range(speaker->elevation_range));
    }
    return layout;
}

[[nodiscard]] bool needs_project_ear_layout(std::string_view layout_id) {
    return layout_id == "4+5+4" || layout_id == "9.1.6";
}

[[nodiscard]] ear::Layout make_ear_layout(std::string_view layout_id, SpeakerGeometry geometry) {
    if (geometry == SpeakerGeometry::apple) {
        if (const auto* layout = render_layouts::find_speaker_layout(layout_id, geometry); layout != nullptr) {
            // Keep libear's ADM nominal positions and known triangulation, while
            // replacing only the real/effective speaker coordinates. In particular,
            // Apple's 5.1.2 top-middle pair cannot itself serve as libear's nominal
            // topology. wav71 uses libear's 0+7+0 order and is remapped to WAVE order
            // after rendering, exactly like the standard profile.
            ear::Layout nominal;
            if (layout_id == "wav71") {
                nominal = ear::getLayout("0+7+0");
            } else if (needs_project_ear_layout(layout_id)) {
                const auto* standard = render_layouts::find_speaker_layout(layout_id, SpeakerGeometry::standard);
                if (standard == nullptr) {
                    throw std::invalid_argument(fmt::format("standard nominal layout '{}' is unavailable", layout_id));
                }
                nominal = make_custom_ear_layout(*standard);
            } else {
                nominal = ear::getLayout(std::string{layout_id});
            }
            return apply_effective_speaker_positions(std::move(nominal), *layout);
        }
        throw std::invalid_argument(fmt::format("Apple speaker geometry is unavailable for layout '{}'", layout_id));
    }
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
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
            consistency::dump("ear.01-object-input.f32",
                              {source.position.azimuth,
                               source.position.elevation,
                               source.position.distance,
                               source.width,
                               source.height,
                               source.depth,
                               source.gain,
                               source.diffuse});
            consistency::dump("ear.02-direct.f64", direct);
            consistency::dump("ear.03-diffuse.f64", diffuse);
#endif
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

Result<void> append_direct_speakers_blocks(const SceneTrackRef& track,
                                           const SceneObject& obj,
                                           ChannelGainInfo& cg,
                                           ear::GainCalculatorDirectSpeakers& direct_speakers_calc,
                                           std::size_t num_out,
                                           const render_common::ResolvedDirectSpeakersMatrix* matrix,
                                           const render_common::LfeRoutingPlan& lfe_routing) {
    for (const auto& ds : track.ds_blocks) {
        BlockGains bg;
        bg.gains.resize(num_out, 0.0);
        bg.diffuse_gains.resize(num_out, 0.0); // DS has no diffuse bus
        bg.start_sample = ds.start_sample;
        bg.end_sample = std::min(ds.end_sample, obj.end_sample);
        bg.jump_position = true;
        const auto lfe_target = render_common::direct_speakers_lfe_target(ds);
        if (lfe_routing.applies_to_22_2 && lfe_target != render_common::LfeTarget::none) {
            bg.gains[render_common::k_22_2_lfe1_index] = lfe_routing.gain(lfe_target, render_common::LfeTarget::lfe1);
            bg.gains[render_common::k_22_2_lfe2_index] = lfe_routing.gain(lfe_target, render_common::LfeTarget::lfe2);
        } else if (lfe_target != render_common::LfeTarget::none) {
            auto meta = direct_speakers_metadata_from_block(ds);
            direct_speakers_calc.calculate(meta, bg.gains);
        } else if (matrix != nullptr) {
            auto route = render_common::direct_speakers_matrix_route_for_block(*matrix, ds);
            if (!route) {
                return tl::unexpected{route.error()};
            }
            for (const auto& target : (*route)->targets) {
                bg.gains[target.output_channel] = static_cast<double>(target.gain);
            }
        } else {
            auto meta = direct_speakers_metadata_from_block(ds);
            direct_speakers_calc.calculate(meta, bg.gains);
        }
        const auto ds_gain = static_cast<double>(ds.gain) * static_cast<double>(obj.gain);
        std::ranges::transform(bg.gains, bg.gains.begin(), [ds_gain](double g) { return g * ds_gain; });
        cg.blocks.push_back(std::move(bg));
    }
    return {};
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
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    for (std::size_t row = 0; row < decode_matrix.size(); ++row) {
        consistency::dump("ear.hoa-decode-row-" + std::to_string(row) + ".f64", decode_matrix[row]);
    }
#endif

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

Result<std::vector<ChannelGainInfo>> build_gain_matrix(const AdmScene& scene,
                                                       const ear::Layout& layout,
                                                       LogSink& logs,
                                                       const render_common::ResolvedDirectSpeakersMatrix* matrix,
                                                       const render_common::LfeRoutingPlan& lfe_routing) {
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
            // Capture the channel's DirectSpeakers label so a per-channel live override can target
            // one bed channel (Objects tracks have no ds_blocks → key stays empty → whole-object).
            if (!track.ds_blocks.empty() && !track.ds_blocks.front().speaker_labels.empty()) {
                cg.speaker_label_key =
                    render_common::canonicalise_speaker_label(track.ds_blocks.front().speaker_labels.front());
            }
            append_object_blocks(track, obj, cg, objects_calc, speakers, num_out, logs, screen_ref_warned);
            auto direct_speakers =
                append_direct_speakers_blocks(track, obj, cg, direct_speakers_calc, num_out, matrix, lfe_routing);
            if (!direct_speakers) {
                return tl::unexpected{direct_speakers.error()};
            }
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

Result<render_common::PreparedPcmMix>
prepare_ear_mix(std::vector<ChannelGainInfo> channels, std::size_t inputs, std::size_t outputs) {
    render_common::PreparedPcmMix result;
    std::vector<MradmDspMixRow> rows;
    std::vector<MradmDspMixBlock> blocks;
    std::vector<double> gains;
    for (auto& channel : channels) {
        result.channels.push_back(
            {channel.input_channel, std::move(channel.object_id), std::move(channel.speaker_label_key)});
        rows.push_back({channel.input_channel, blocks.size(), channel.blocks.size(), 1.0F});
        for (const auto& block : channel.blocks) {
            if (block.gains.size() != outputs || block.diffuse_gains.size() != outputs) {
                return make_error(ErrorCode::invalid_argument, "EAR gain width mismatch");
            }
            // Object-duration clipping can put the end before the start. Preserve an empty block
            // (and its gains) so later blocks retain the legacy interpolation predecessor.
            blocks.push_back({block.start_sample,
                              std::max(block.start_sample, block.end_sample),
                              block.interp_length_samples.value_or(0),
                              (block.jump_position ? 1U : 0U) | (block.smoothable_object ? 2U : 0U) |
                                  (block.interp_length_samples ? 4U : 0U)});
            gains.insert(gains.end(), block.gains.begin(), block.gains.end());
            gains.insert(gains.end(), block.diffuse_gains.begin(), block.diffuse_gains.end());
        }
        std::vector<BlockGains>{}.swap(channel.blocks);
    }
    auto plan = dsp::PcmMixPlan::create(inputs, outputs, rows, blocks, {}, gains, true);
    if (!plan) {
        return tl::unexpected{plan.error()};
    }
    result.plan = std::move(*plan);
    return result;
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

// The offline and streaming paths share Rust mixing and post-processing. Caller-owned
// interleaved buffers feed metering/I/O; Rust owns all convolution, delay and cursor state.
// NOLINTNEXTLINE(readability-function-size): retain the existing EAR post-processing buffer boundaries.
void render_ear_block(dsp::PcmMixer& mix,
                      dsp::EarPostProcessor& post,
                      const std::vector<float>& in_block,
                      std::vector<float>& out_block,
                      std::vector<float>& diffuse_in,
                      uint64_t frames_done,
                      uint64_t frames_now,
                      uint16_t num_out_ch,
                      const std::string& output_layout) {
    mix.ear(in_block, out_block, diffuse_in, frames_done, static_cast<std::size_t>(frames_now));

    post.process(out_block, diffuse_in, static_cast<std::size_t>(frames_now));
    if (output_layout == "wav71") {
        remap_wav71_to_wave_order(out_block, frames_now, num_out_ch);
    }
}

// libear owns filter design; Rust retains the raw FIRs for independent output instances.
Result<dsp::EarFilters> prepare_ear_filters(const ear::Layout& layout) {
    const auto raw = ear::designDecorrelators<float>(layout);
    if (raw.size() != layout.channels().size() ||
        raw.size() > std::numeric_limits<std::size_t>::max() / dsp::EarFilters::k_taps) {
        return make_error(ErrorCode::render_failed, "Unexpected EAR filter count");
    }
    std::vector<float> firs;
    firs.reserve(raw.size() * dsp::EarFilters::k_taps);
    for (const auto& filter : raw) {
        if (filter.size() != dsp::EarFilters::k_taps) {
            return make_error(ErrorCode::render_failed, "Unexpected EAR filter length");
        }
        firs.insert(firs.end(), filter.begin(), filter.end());
    }
    return dsp::EarFilters::create(raw.size(), firs, static_cast<std::size_t>(ear::decorrelatorCompensationDelay()));
}

// Immutable, reusable EAR state: the resolved libear layout and the per-object gain
// matrix and raw FIR bank. FFT spectra/history remain per output instance so the same
// prepared metadata supports different block capacities. ear::Layout stays in this TU.
struct EarPrepared final : IPreparedRender {
    ear::Layout layout;
    render_common::PreparedPcmMix gain_matrix;
    dsp::EarFilters filters;
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
        auto reader = audio::RenderInputReader::open(plan.input_path,
                                                     plan.scene.info.source_kind == SceneSourceKind::channel_bed);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        (void) logs;
        try {
            return std::unique_ptr<EarStream>{new EarStream(prepared, std::move(*reader), plan)};
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
                if (auto result = render_block(); !result) {
                    return tl::unexpected{result.error()};
                }
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
        post_.reset();
        mix_.reset();
        frames_done_ = std::min(frame, total_frames_);
        if (auto result = reader_->seek_frame(frames_done_); !result) {
            return tl::unexpected{result.error()};
        }
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        std::ranges::fill(live_gain_targets_, 1.0F);
        // The override is projected object → input channel (each channel scaled by its owning
        // object's gain). A per-channel override (DirectSpeakers speaker_label) targets one bed
        // channel; an empty-label override scales every channel of the object (whole-object).
        for (const auto& cg : prepared_.gain_matrix) {
            if (cg.input_channel >= live_gain_targets_.size()) {
                continue;
            }
            if (const auto g =
                    render_common::resolve_live_channel_gain(overrides, cg.object_id, cg.speaker_label_key)) {
                live_gain_targets_[cg.input_channel] = *g;
            }
        }
        live_gain_smoother_.set_targets(live_gain_targets_);
    }

    [[nodiscard]] uint32_t out_channels() const override { return num_out_ch_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

  private:
    EarStream(const EarPrepared& prepared, std::unique_ptr<audio::RenderInputReader> reader, const RenderPlan& plan)
        : prepared_(prepared), reader_(std::move(reader)), num_in_ch_(plan.scene.info.num_channels),
          num_out_ch_(static_cast<uint16_t>(prepared.layout.channels().size())),
          sample_rate_(plan.scene.info.sample_rate), total_frames_(plan.scene.info.num_frames),
          output_layout_(plan.output_layout), object_smoothing_frames_(plan.object_smoothing_frames),
          k_block_size_(std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          col_stride_(static_cast<std::size_t>(std::max<uint64_t>(1024U, plan.object_smoothing_frames))),
          mix_(prepared.gain_matrix.plan,
               col_stride_,
               static_cast<uint64_t>(plan.scene.info.sample_rate) * plan.default_interp_ms / 1000U,
               object_smoothing_frames_ > 0),
          post_(prepared.filters, k_block_size_),
          diffuse_in_(static_cast<std::size_t>(num_out_ch_) * col_stride_, 0.0F),
          in_block_(static_cast<std::size_t>(plan.scene.info.num_channels) * col_stride_, 0.0F),
          live_gain_targets_(plan.scene.info.num_channels, 1.0F),
          live_gain_smoother_(plan.scene.info.num_channels, plan.scene.info.sample_rate) {}

    void apply_live_gain(uint64_t frames_now) {
        live_gain_smoother_.apply(in_block_.data(), static_cast<std::size_t>(frames_now));
    }

    Result<void> render_block() {
        const uint64_t frames_now = std::min<uint64_t>(k_block_size_, total_frames_ - frames_done_);
        const auto read_result = reader_->read(in_block_.data(), frames_now);
        if (!read_result) {
            return tl::unexpected{read_result.error()};
        }
        if (*read_result != frames_now) {
            return make_error(ErrorCode::io_error, "short render input read");
        }
        apply_live_gain(frames_now);
        fifo_.assign(static_cast<std::size_t>(num_out_ch_) * frames_now, 0.0F);
        fifo_read_ = 0;
        render_ear_block(
            mix_, post_, in_block_, fifo_, diffuse_in_, frames_done_, frames_now, num_out_ch_, output_layout_);
        frames_done_ += frames_now;
        return {};
    }

    const EarPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<audio::RenderInputReader> reader_;
    uint16_t num_in_ch_;
    uint16_t num_out_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    std::string output_layout_;
    uint64_t object_smoothing_frames_;
    uint64_t k_block_size_;
    std::size_t col_stride_;
    dsp::PcmMixer mix_;
    dsp::EarPostProcessor post_;
    std::vector<float> diffuse_in_;
    std::vector<float> in_block_;
    std::vector<float> live_gain_targets_; // per-input-channel target multiplier (1.0 = neutral)
    render_common::InterleavedLiveGainSmoother live_gain_smoother_;
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
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic &&
        plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::matrix) {
        return make_error(
            ErrorCode::unsupported, "EAR renderer supports only automatic or matrix DirectSpeakers routing", {});
    }
    try {
        auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "ear");
        if (!lfe_routing) {
            return tl::unexpected{lfe_routing.error()};
        }
        ear::Layout layout = make_ear_layout(plan.output_layout, plan.speaker_geometry);
        logs.log(LogLevel::info,
                 "ear",
                 fmt::format("speaker geometry: {}",
                             plan.speaker_geometry == SpeakerGeometry::apple ? "apple" : "standard"));
        std::optional<render_common::ResolvedDirectSpeakersMatrix> resolved_matrix;
        if (plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::matrix) {
            if (plan.direct_speakers_matrix == nullptr) {
                return make_error(ErrorCode::invalid_argument,
                                  "DirectSpeakers matrix routing requires a parsed matrix");
            }
            const auto targets = direct_speaker_targets(layout);
            auto resolved = render_common::resolve_direct_speakers_matrix_targets(
                *plan.direct_speakers_matrix, targets, plan.output_layout);
            if (!resolved) {
                return tl::unexpected{resolved.error()};
            }
            resolved_matrix = std::move(*resolved);
            logs.log(LogLevel::info, "ear", "DirectSpeakers routing: matrix");
        }
        auto gain_matrix =
            build_gain_matrix(plan.scene, layout, logs, resolved_matrix ? &*resolved_matrix : nullptr, *lfe_routing);

        if (!gain_matrix) {
            return tl::unexpected{gain_matrix.error()};
        }
        if (gain_matrix->empty()) {
            logs.log(LogLevel::warning, "ear", "no renderable tracks found (all muted?), writing silence");
        }

        const auto num_in_ch = plan.scene.info.num_channels;
        const auto invalid_channel =
            std::ranges::find_if(*gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
        if (invalid_channel != gain_matrix->end()) {
            return make_error(ErrorCode::render_failed,
                              fmt::format("track channel index {} is outside input channel count {}",
                                          invalid_channel->input_channel,
                                          num_in_ch),
                              "input=" + plan.input_path);
        }

        auto prepared = std::make_shared<EarPrepared>();
        prepared->layout = std::move(layout);
        auto compiled = prepare_ear_mix(std::move(*gain_matrix), num_in_ch, prepared->layout.channels().size());
        if (!compiled) {
            return tl::unexpected{compiled.error()};
        }
        prepared->gain_matrix = std::move(*compiled);
        auto filters = prepare_ear_filters(prepared->layout);
        if (!filters) {
            return tl::unexpected{filters.error()};
        }
        prepared->filters = std::move(*filters);
        return std::static_pointer_cast<IPreparedRender>(prepared);
    } catch (const std::invalid_argument& e) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported output layout '{}': {}", plan.output_layout, e.what()),
                          "layout=" + plan.output_layout);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::render_failed,
                          std::string{"failed to prepare EAR renderer: "} + e.what(),
                          "layout=" + plan.output_layout);
    }
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

        dsp::EarPostProcessor post(prepared->filters, static_cast<std::size_t>(k_block_size));

        // Open file for audio only — ADM metadata comes from plan.scene.
        auto reader_res = audio::RenderInputReader::open(plan.input_path,
                                                         plan.scene.info.source_kind == SceneSourceKind::channel_bed);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto reader = std::move(*reader_res);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        dsp::PcmMixer mix(gain_matrix.plan,
                          static_cast<std::size_t>(k_block_size),
                          k_default_interp,
                          plan.object_smoothing_frames > 0);

        // Inline loudness + true-peak measurement (BS.1770-4 / EBU R128).
        auto lufs_st =
            dsp::Meter::create(num_out_ch, static_cast<uint32_t>(sample_rate), dsp::MeterMode::integrated_true_peak);
        if (!lufs_st) {
            return tl::unexpected{lufs_st.error()};
        }

        // Interleaved buffers used for I/O and the decorrelator.
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> diffuse_in(num_out_ch * k_block_size); // interleaved diffuse

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
            if (auto result = reader->seek_frame(start_pos); !result) {
                return tl::unexpected{result.error()};
            }
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

            const auto read_result = reader->read(in_block.data(), frames_now);
            if (!read_result) {
                return tl::unexpected{read_result.error()};
            }
            if (*read_result != frames_now) {
                return make_error(ErrorCode::io_error, "short render input read");
            }

            render_ear_block(
                mix, post, in_block, out_block, diffuse_in, frames_done, frames_now, num_out_ch, plan.output_layout);

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
                    auto* state = &*lufs_st;
                    const float* data = out_block.data() + (meter_off * num_out_ch);
                    const auto frame_count = meter_count;
                    meter_pending.at(buf_idx) = meter.post([state, data, frame_count] {
                        if (const auto result = state->add_frames(data, frame_count); !result) {
                            throw std::runtime_error(result.error().message);
                        }
                    });
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
            if (const auto loudness = lufs_st->integrated(); loudness && std::isfinite(*loudness)) {
                metrics.measured_lufs = *loudness;
            }
            if (const auto peak = lufs_st->max_true_peak(); peak && *peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(*peak);
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
