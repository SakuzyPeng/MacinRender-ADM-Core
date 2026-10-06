#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hoa.h"
#include "meter.h"
// clang-format off
#include "dsp.h"
// clang-format on

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_hoa.h"

#include "consistency_trace.h"
#include "render_common.h"

namespace mradm {

namespace {

constexpr std::size_t k_hoa3_channels = 16;
constexpr uint16_t k_hoa3_channels_u16 = 16;

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

struct ChannelBinding {
    uint16_t input_channel{};
    std::string object_id;
};
struct HoaPrepared final : IPreparedRender {
    std::vector<ChannelBinding> gain_matrix;
    dsp::HoaPlan numeric;
    uint16_t input_channels{};
    uint32_t sample_rate{};
};

MradmHoaSource numeric_source(const SceneObjectBlock& block) {
    const auto& p = block.position;
    MradmHoaSource source{};
    if (p.cartesian) {
        source.position[0] = p.x;
        source.position[1] = p.y;
        source.position[2] = p.z;
    } else {
        source.position[0] = p.azimuth;
        source.position[1] = p.elevation;
        source.position[2] = p.distance;
    }
    source.cartesian = p.cartesian ? 1U : 0U;
    source.width = block.width;
    source.height = block.height;
    source.depth = block.depth;
    source.gain = block.gain;
    source.diffuse = block.diffuse;
    return source;
}

// Keep scene traversal and the original C++ sort order; Rust compiles coefficients before applying this permutation.
// NOLINTNEXTLINE(readability-function-size): preserve scene traversal and paired semantic/transport preparation.
Result<void> build_gain_matrix(const AdmScene& scene, LogSink& logs, HoaPrepared& prepared) {
    struct BlockOrder {
        std::size_t index;
        uint64_t start_sample;
    };
    struct PendingChannel {
        ChannelBinding binding;
        std::vector<BlockOrder> order;
    };
    std::map<uint16_t, PendingChannel> by_channel;
    std::vector<MradmHoaBlock> blocks;
    std::vector<MradmHoaSource> sources;
    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index) {
                continue;
            }
            const auto in_ch = *track.channel_index;
            if (in_ch >= scene.info.num_channels) {
                return make_error(ErrorCode::render_failed,
                                  fmt::format("track channel index {} is outside input channel count {}",
                                              in_ch,
                                              scene.info.num_channels));
            }
            auto& cg = by_channel[in_ch];
            cg.binding = {in_ch, obj.id};
            for (const auto& raw : track.blocks) {
                SceneObjectBlock base = raw;
                if (obj.position_offset) {
                    base.position = apply_position_offset(base.position, *obj.position_offset);
                }
                const auto expanded = expand_object_divergence(base);
                const auto offset = sources.size();
                std::ranges::transform(expanded, std::back_inserter(sources), numeric_source);
                cg.order.push_back({blocks.size(), raw.start_sample});
                blocks.push_back({raw.start_sample,
                                  std::min(raw.end_sample, obj.end_sample),
                                  raw.interp_length_samples.value_or(0),
                                  offset,
                                  expanded.size(),
                                  obj.gain,
                                  0U,
                                  (raw.jump_position ? 1U : 0U) | (raw.interp_length_samples ? 2U : 0U)});
            }
            for (const auto& ds : track.ds_blocks) {
                const bool lfe = render_common::direct_speakers_block_is_lfe(ds);
                float azimuth = ds.azimuth;
                float elevation = ds.elevation;
                if (!lfe && !ds.has_position) {
                    std::optional<std::pair<float, float>> found;
                    for (const auto& label : ds.speaker_labels) {
                        found = parse_speaker_label(label);
                        if (found) {
                            break;
                        }
                    }
                    if (!found) {
                        logs.log(
                            LogLevel::warning,
                            "hoa-encode",
                            fmt::format("DirectSpeakers channel {} has no position and no parseable label; skipping",
                                        in_ch));
                        continue;
                    }
                    azimuth = found->first;
                    elevation = found->second;
                }
                const auto offset = sources.size();
                sources.push_back({{azimuth, elevation, 1}, 0, 0, 0, 0, ds.gain, 0});
                cg.order.push_back({blocks.size(), ds.start_sample});
                blocks.push_back({ds.start_sample,
                                  std::min(ds.end_sample, obj.end_sample),
                                  0,
                                  offset,
                                  1,
                                  obj.gain,
                                  lfe ? 2U : 1U,
                                  1U});
            }
        }
    }
    std::vector<MradmHoaRow> rows;
    std::vector<std::size_t> order;
    for (auto& [channel, cg] : by_channel) {
        std::ranges::sort(cg.order, {}, &BlockOrder::start_sample);
        rows.push_back({channel, order.size(), cg.order.size()});
        std::ranges::transform(cg.order, std::back_inserter(order), [](const auto& block) { return block.index; });
        prepared.gain_matrix.push_back(std::move(cg.binding));
    }
    MradmHoaTrace* trace_ptr = nullptr;
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    MradmHoaTrace trace{};
    trace_ptr = &trace;
#endif
    auto compiled = dsp::HoaPlan::create(scene.info.num_channels, rows, blocks, order, sources, trace_ptr);
    if (!compiled) {
        return tl::unexpected{compiled.error()};
    }
    prepared.numeric = std::move(*compiled);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    if ((trace.flags & 1U) != 0) {
        consistency::dump("hoa.01-polar.f32", std::span<const float>{trace.polar});
    }
    if ((trace.flags & 2U) != 0) {
        consistency::dump("hoa.02-direction.f32", std::span<const float>{trace.direction});
        consistency::dump("hoa.03-normalized.f32", std::span<const float>{trace.normalized});
        consistency::dump("hoa.04-coefficients.f32", std::span<const float>{trace.coefficients});
    }
#endif
    return {};
}

// Realtime streaming HOA session over the same prepared encode gain matrix as
// render_window. It encodes k_block_size-aligned blocks via the same Rust encoder the
// offline path uses (carrying the diffuse decorrelation delay lines across blocks) into a
// FIFO of k_hoa3_channels SH channels — the encoded ambisonic signal, identical to what
// render_window writes (the AllRAD 7.1.4 decode is metering-only and not part of the
// output). A gap-free run from frame 0 is bit-identical to render_window. seek() resets the
// diffuse delay lines (a small discontinuity, acceptable for monitoring), seeding the
// circular write position to frame % delay-len to match the offline windowed indexing.
// set_overrides applies live per-object gain by pre-scaling the matching input channels
// before the (linear) HOA encode — equal to scaling the object gain, and it scales the
// object's direct + diffuse alike. Topology scales are not yet wired for HOA.
class HoaStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<HoaStream>>
    create(const HoaPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        if (plan.scene.info.num_channels != prepared.input_channels ||
            plan.scene.info.sample_rate != prepared.sample_rate) {
            return make_error(ErrorCode::invalid_argument, "HOA prepared/input format mismatch");
        }
        auto reader = audio::RenderInputReader::open(plan.input_path,
                                                     plan.scene.info.source_kind == SceneSourceKind::channel_bed);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        (void) logs;
        auto encoder = dsp::HoaEncoder::create(prepared.numeric,
                                               std::max<std::size_t>(1024U, plan.object_smoothing_frames),
                                               uint64_t{plan.scene.info.sample_rate} * plan.default_interp_ms / 1000U,
                                               plan.object_smoothing_frames > 0);
        if (!encoder) {
            return tl::unexpected{encoder.error()};
        }
        return std::unique_ptr<HoaStream>{new HoaStream(prepared, std::move(*reader), plan, std::move(*encoder))};
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        if (frames > out.size() / k_hoa3_channels) {
            return make_error(ErrorCode::invalid_argument, "HOA output buffer is too small");
        }
        try {
            std::size_t produced = 0;
            while (produced < frames) {
                if (fifo_read_ >= fifo_.size()) {
                    if (frames_done_ >= total_frames_) {
                        break;
                    }
                    auto status = render_block();
                    if (!status) {
                        return tl::unexpected{status.error()};
                    }
                    if (fifo_read_ >= fifo_.size()) {
                        break;
                    }
                }
                const std::size_t avail = (fifo_.size() - fifo_read_) / k_hoa3_channels;
                const std::size_t take = std::min(frames - produced, avail);
                std::copy_n(
                    fifo_.data() + fifo_read_, take * k_hoa3_channels, out.data() + (produced * k_hoa3_channels));
                fifo_read_ += take * k_hoa3_channels;
                produced += take;
            }
            return produced;
        } catch (const std::exception& error) {
            return make_error(ErrorCode::io_error, error.what(), "HOA stream");
        }
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        frames_done_ = std::min(frame, total_frames_);
        encoder_.reset(frames_done_);
        if (auto result = reader_->seek_frame(frames_done_); !result) {
            return tl::unexpected{result.error()};
        }
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        std::unordered_map<std::string, float> live;
        for (const auto& ov : overrides.objects) {
            live[ov.object_id] = ov.mute ? 0.0F : std::pow(10.0F, ov.gain_db / 20.0F);
        }
        std::ranges::fill(live_gain_targets_, 1.0F);
        // Semantic boundary: the override is projected object → input channel (each channel
        // scaled by its owning object's gain). ADM's gain matrix is one object per input
        // channel, so this is exact today; if independently-overridable objects ever shared
        // one input channel they would scale together (last writer wins) — revisit then.
        for (const auto& cg : prepared_.gain_matrix) {
            if (const auto it = live.find(cg.object_id);
                it != live.end() && cg.input_channel < live_gain_targets_.size()) {
                live_gain_targets_[cg.input_channel] = it->second;
            }
        }
        live_gain_smoother_.set_targets(live_gain_targets_);
    }

    [[nodiscard]] uint32_t out_channels() const override { return k_hoa3_channels_u16; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return "hoa3"; }

  private:
    HoaStream(const HoaPrepared& prepared,
              std::unique_ptr<audio::RenderInputReader> reader,
              const RenderPlan& plan,
              dsp::HoaEncoder encoder)
        : prepared_(prepared), reader_(std::move(reader)), num_in_ch_(plan.scene.info.num_channels),
          sample_rate_(plan.scene.info.sample_rate), total_frames_(plan.scene.info.num_frames),
          k_block_size_(std::max<uint64_t>(1024U, plan.object_smoothing_frames)), encoder_(std::move(encoder)),
          in_block_(static_cast<std::size_t>(plan.scene.info.num_channels) *
                    std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          live_gain_targets_(plan.scene.info.num_channels, 1.0F),
          live_gain_smoother_(plan.scene.info.num_channels, plan.scene.info.sample_rate) {
        fifo_.reserve(static_cast<std::size_t>(k_block_size_) * k_hoa3_channels);
    }

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
            return make_error(ErrorCode::io_error, "short input read while encoding HOA");
        }
        apply_live_gain(frames_now);
        fifo_.assign(k_hoa3_channels * static_cast<std::size_t>(frames_now), 0.0F);
        fifo_read_ = 0;
        const auto frames = static_cast<std::size_t>(frames_now);
        auto status = encoder_.process(
            std::span<const float>{in_block_.data(), frames * num_in_ch_}, fifo_, frames_done_, frames);
        if (!status) {
            return status;
        }
        frames_done_ += frames_now;
        return {};
    }

    const HoaPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<audio::RenderInputReader> reader_;
    uint16_t num_in_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    uint64_t k_block_size_;
    dsp::HoaEncoder encoder_;
    std::vector<float> in_block_;
    std::vector<float> live_gain_targets_; // per-input-channel target multiplier (1.0 = neutral)
    render_common::InterleavedLiveGainSmoother live_gain_smoother_;
    std::vector<float> fifo_;
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const HoaPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "hoa-encode: open_stream received an incompatible prepared state", {});
        }
        auto stream = HoaStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport HoaRenderer::capabilities() const {
    return hoa_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> HoaRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic) {
        return make_error(
            ErrorCode::unsupported, "HOA renderer does not support explicit DirectSpeakers routing; use automatic", {});
    }
    auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "hoa-encode");
    if (!lfe_routing) {
        return tl::unexpected{lfe_routing.error()};
    }
    if (plan.output_layout != "hoa3") {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported HOA output layout '{}'; supported: hoa3", plan.output_layout),
                          {});
    }

    if (plan.scene.info.sample_rate == 0) {
        return make_error(ErrorCode::invalid_argument, "HOA requires a nonzero sample rate");
    }
    auto prepared = std::make_shared<HoaPrepared>();
    prepared->input_channels = plan.scene.info.num_channels;
    prepared->sample_rate = plan.scene.info.sample_rate;
    auto built = build_gain_matrix(plan.scene, logs, *prepared);
    if (!built) {
        return tl::unexpected{built.error()};
    }
    if (prepared->gain_matrix.empty()) {
        logs.log(LogLevel::warning, "hoa-encode", "no renderable tracks found (all muted?), writing silence");
    }
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> HoaRenderer::render_window(const IPreparedRender& prep,
                                                 const RenderPlan& plan,
                                                 ProgressSink& progress,
                                                 LogSink& logs) { // NOLINT(readability-function-size)
    const auto* prepared = dynamic_cast<const HoaPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "hoa-encode: render_window received an incompatible prepared state", {});
    }
    if (plan.scene.info.num_channels != prepared->input_channels ||
        plan.scene.info.sample_rate != prepared->sample_rate) {
        return make_error(ErrorCode::invalid_argument, "HOA prepared/input format mismatch");
    }
    const auto& gain_matrix = prepared->gain_matrix;

    const auto& info = plan.scene.info;
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
    constexpr uint16_t k_num_out = k_hoa3_channels_u16;

    constexpr int k_714_ch = 12;
    constexpr std::size_t k_714_ch_sz = 12;

    try {
        logs.log(LogLevel::info,
                 "hoa-encode",
                 fmt::format("encoding {} input channels (Objects + DirectSpeakers) → HOA3 ({} ch), {} frames",
                             gain_matrix.size(),
                             k_num_out,
                             num_frames));
        progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "encoding HOA"});

        auto reader_res = audio::RenderInputReader::open(plan.input_path,
                                                         plan.scene.info.source_kind == SceneSourceKind::channel_bed);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto reader = std::move(*reader_res);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, k_num_out, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        std::vector<float> decoded_block(k_714_ch_sz * k_block_size);
        auto encoder = dsp::HoaEncoder::create(prepared->numeric,
                                               static_cast<std::size_t>(k_block_size),
                                               k_default_interp,
                                               plan.object_smoothing_frames > 0);
        if (!encoder) {
            return tl::unexpected{encoder.error()};
        }
        auto measure = dsp::HoaMeterPreprocessor::create(
            prepared->numeric, static_cast<std::size_t>(k_block_size), k_default_interp);
        if (!measure) {
            return tl::unexpected{measure.error()};
        }

        using Ch = dsp::MeterChannel;
        constexpr std::array<Ch, 12> meter_map{Ch::left,
                                               Ch::right,
                                               Ch::center,
                                               Ch::unused,
                                               Ch::side_left,
                                               Ch::side_right,
                                               Ch::rear_left,
                                               Ch::rear_right,
                                               Ch::top_front_left,
                                               Ch::top_front_right,
                                               Ch::top_rear_left,
                                               Ch::top_rear_right};
        auto lufs_st = dsp::Meter::create(static_cast<uint32_t>(k_714_ch),
                                          static_cast<uint32_t>(sample_rate),
                                          dsp::MeterMode::integrated_true_peak,
                                          meter_map);
        if (!lufs_st) {
            return tl::unexpected{lufs_st.error()};
        }

        // Separate TP tracker for LFE channels. LFE is still encoded W-only in the HOA
        // output, but it is subtracted from the HOA measurement buffer before the 7.1.4
        // decode so it cannot contribute to LUFS/spatial TP. Its peak is measured on this
        // mono TP-only state and merged with the spatial decode peak at the end.
        std::optional<dsp::Meter> lfe_tp_st;
        std::vector<float> lfe_mix_block(static_cast<std::size_t>(k_block_size));
        const bool has_lfe = prepared->numeric.has_lfe();
        if (has_lfe) {
            auto made = dsp::Meter::create(1U, static_cast<uint32_t>(sample_rate), dsp::MeterMode::true_peak);
            if (!made) {
                return tl::unexpected{made.error()};
            }
            lfe_tp_st.emplace(std::move(*made));
        }

        // Loudness / true-peak measurement (LFE TP + 7.1.4 decode + two Rust meters) is run on a
        // background thread so it overlaps the next block's HOA encode. The measurement only reads the
        // input and output buffers (both double-buffered below) plus const scene data; its scratch and
        // meter states are touched solely by the worker. Running blocks in FIFO order keeps the
        // measured loudness / true peak bit-identical to the inline version.
        const auto measure_block = [&](const float* in_data, const float* out_data, uint64_t fd, uint64_t fn) {
            const auto frames = static_cast<std::size_t>(fn);
            auto status = measure->process(std::span<const float>{in_data, frames * num_in_ch},
                                           std::span<const float>{out_data, frames * k_num_out},
                                           decoded_block,
                                           lfe_mix_block,
                                           fd,
                                           frames);
            if (!status) {
                throw std::runtime_error(status.error().message);
            }
            if (lfe_tp_st) {
                auto result = lfe_tp_st->add_frames(lfe_mix_block.data(), frames);
                if (!result) {
                    throw std::runtime_error(result.error().message);
                }
            }
            auto result = lufs_st->add_frames(decoded_block.data(), frames);
            if (!result) {
                throw std::runtime_error(result.error().message);
            }
        };

        // Double-buffer the input and output blocks so the next block can be encoded while the meter
        // still reads the previous block's buffers; reuse waits on the outstanding measurement future.
        constexpr std::size_t k_num_buffers = 2;
        std::array<std::vector<float>, k_num_buffers> in_buffers;
        std::array<std::vector<float>, k_num_buffers> out_buffers;
        for (auto& buffer : in_buffers) {
            buffer.assign(static_cast<std::size_t>(num_in_ch) * k_block_size, 0.0F);
        }
        for (auto& buffer : out_buffers) {
            buffer.assign(static_cast<std::size_t>(k_num_out) * k_block_size, 0.0F);
        }
        std::array<std::future<void>, k_num_buffers> meter_pending;
        render_common::SerialWorker meter;
        std::size_t buf_idx = 0;

        // On-demand output window (RenderPlan::render_window). HOA's diffuse path has a
        // 1024-frame delay line and its smoothing samples gains at block
        // edges, so blocks are processed on the same k_block_size grid as a full render
        // and one aligned block (>= the 1024-tap delay) is pre-rolled before the window.
        // The delay line is circular, indexed by absolute frame mod 1024,
        // so each state's write_pos is seeded to start_pos % 1024 to match
        // the full render exactly; the pre-roll block then refills the line. Direct (non-
        // diffuse) gains are closed-form per absolute frame. When not windowed,
        // win_start=0 / win_end=num_frames reproduces the full-timeline encode.
        const bool windowed = plan.render_window.has_value();
        const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
        const uint64_t win_end =
            windowed ? win_start + std::min(plan.render_window->frame_count, num_frames - win_start) : num_frames;
        uint64_t start_pos = 0;
        if (windowed && win_start >= k_block_size) {
            start_pos = ((win_start / k_block_size) - 1) * k_block_size; // one aligned pre-roll block
        }
        if (start_pos > 0) {
            if (auto result = reader->seek_frame(start_pos); !result) {
                return tl::unexpected{result.error()};
            }
            encoder->reset(start_pos);
            measure->reset(start_pos);
        }
        const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
        const auto progress_span = static_cast<double>(progress_total);
        uint64_t frames_done = start_pos;

        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(k_num_out) * frames_now;

            // Sub-range of this block inside the output window [win_start, win_end).
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

            // Reclaim this block's buffers once the meter has finished its previous use of them.
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            std::vector<float>& in_block = in_buffers.at(buf_idx);
            std::vector<float>& out_block = out_buffers.at(buf_idx);

            const auto read_result = reader->read(in_block.data(), frames_now);
            if (!read_result) {
                return tl::unexpected{read_result.error()};
            }
            if (*read_result != frames_now) {
                return make_error(
                    ErrorCode::io_error, "short input read while encoding HOA", "input=" + plan.input_path);
            }
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            auto status = encoder->process(
                std::span<const float>{in_block.data(), static_cast<std::size_t>(frames_now) * num_in_ch},
                std::span<float>{out_block.data(), out_samples},
                frames_done,
                static_cast<std::size_t>(frames_now));
            if (!status) {
                return tl::unexpected{status.error()};
            }

            // Write only the in-window frames; pre-roll blocks (emit == false) warm the
            // diffuse delay line but are not written.
            if (emit && writer.write(out_block.data() + (emit_off * k_num_out), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while encoding HOA", "output=" + plan.output_path);
            }

            // Offload loudness / true-peak measurement to the background meter (overlaps next block).
            // Windowed: measure exactly the written frames. Otherwise honor meter_window. Either way
            // pass the absolute start frame so the LFE gain lookups stay correct.
            if (lufs_st || lfe_tp_st) {
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
                    const float* in_data = in_block.data() + (meter_off * num_in_ch);
                    const float* out_data = out_block.data() + (meter_off * k_num_out);
                    const uint64_t fd = frames_done + meter_off;
                    const uint64_t fn = meter_count;
                    meter_pending.at(buf_idx) = meter.post(
                        [&measure_block, in_data, out_data, fd, fn] { measure_block(in_data, out_data, fd, fn); });
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
                                  "encoding"});
        }

        // All audio is written; wait for outstanding measurements before reading global metrics.
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        logs.log(LogLevel::info,
                 "hoa-encode",
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
            double max_peak = 0.0;
            if (const auto peak = lufs_st->max_true_peak(); peak) {
                max_peak = *peak;
            }
            if (lfe_tp_st) {
                if (const auto peak = lfe_tp_st->max_true_peak(); peak) {
                    max_peak = std::max(max_peak, *peak);
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
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.supports_render_window = true; // block-aligned seek + 1 pre-roll block (diffuse delay line)
    r.supported_layouts = {
        {"hoa3", "HOA 3rd Order (16ch, ACN/SN3D)", 16, true, 0, false},
    };
    return r;
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
