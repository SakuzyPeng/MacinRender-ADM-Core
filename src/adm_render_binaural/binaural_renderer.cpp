#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

#include "../adm_audio/wav_backend.h"
#include "hrtf_state.h"
#include "render_common.h"
#include "scene_handle.h"
#include "speaker_pcm.h"
#include "stream_dsp.h"

namespace mradm {

using namespace binaural_internal;

// The renderer's adapters. The HRTF state (4.7 A), the source and spreader plan, the per-source
// OLA convolution and the offline loop (4.7 B) are Rust mradm-render `binaural_hrtf`, `binaural`
// and `binaural_render`.
namespace binaural_internal {
namespace {

constexpr uint64_t k_min_block_size = 1024U;
constexpr uint64_t k_diffuse_delay_len = 32U;

constexpr std::size_t k_topology_diffuse = 0U;
constexpr std::size_t k_topology_width = 1U;
constexpr std::size_t k_topology_height = 2U;
constexpr std::size_t k_topology_depth = 3U;
constexpr std::size_t k_topology_divergence = 4U;
constexpr uint64_t k_live_topology_min_interval_ms = 100U;
using LiveTopologyScales = std::array<float, 5>;

[[nodiscard]] uint32_t numerical_spread(BinauralSpreadMode mode) noexcept {
    switch (mode) {
    case BinauralSpreadMode::none:
        return 1U;
    case BinauralSpreadMode::saf_spreader:
        return 2U;
    default:
        return 0U;
    }
}

// The HRTF state and the Rust plan, reused across render_window() calls (PreviewSession scrubbing);
// the per-output convolution and spreader state is created per render.
struct BinauralPrepared final : IPreparedRender {
    std::shared_ptr<const HrtfState> bs;
    std::shared_ptr<render_common::RustPrepared> rust;
    MradmRenderBinauralInfo info{};
    uint64_t render_block_size{0};
};

// Offline windows and file-stream seeks must rebuild history on the same absolute block grid.
[[nodiscard]] uint64_t ola_preroll_start(uint64_t target, uint64_t block_size, uint64_t overlap_len) {
    const auto warmup_frames = std::max<uint64_t>({block_size, overlap_len, k_diffuse_delay_len});
    const uint64_t warmup_blocks = (warmup_frames + block_size - 1U) / block_size;
    const uint64_t target_block = target / block_size;
    return target_block > warmup_blocks ? (target_block - warmup_blocks) * block_size : 0;
}

// A source list of the preparation (rebuilt with live topology scales) with its Rust OLA state.
class SourceBank {
  public:
    [[nodiscard]] Result<void> rebuild(const BinauralPrepared& prepared,
                                       const std::unordered_map<std::string, LiveTopologyScales>& topo,
                                       const std::string& input_path) {
        std::vector<MradmRenderBinauralScales> scales;
        scales.reserve(topo.size());
        for (const auto& [object_key, value] : topo) {
            scales.push_back({render_common::text(object_key), {value[0], value[1], value[2], value[3], value[4]}});
        }
        MradmBinauralBank* raw = handle_.get();
        const MradmRenderBinauralSourceKey* keys = nullptr;
        std::size_t keys_len = 0;
        render_common::DriverError error;
        if (const auto code = mradm_render_binaural_bank_create(prepared.rust->handle.get(),
                                                                scales.data(),
                                                                scales.size(),
                                                                &raw,
                                                                &keys,
                                                                &keys_len,
                                                                error.location(),
                                                                error.message(),
                                                                error.message_len());
            code != 0) {
            return error.error(code, input_path);
        }
        if (!handle_) {
            handle_.reset(raw);
        }
        keys_ = {keys, keys_len};
        return {};
    }

    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }
    [[nodiscard]] std::string_view object_id(std::size_t i) const noexcept { return view(keys_[i].object_id); }
    [[nodiscard]] std::string_view label_key(std::size_t i) const noexcept { return view(keys_[i].label_key); }
    void reset() noexcept { mradm_render_binaural_bank_reset(handle_.get()); }
    [[nodiscard]] MradmBinauralBank* get() const noexcept { return handle_.get(); }

  private:
    [[nodiscard]] static std::string_view view(MradmAdmText t) noexcept {
        return {reinterpret_cast<const char*>(t.data), t.len};
    }
    std::unique_ptr<MradmBinauralBank, decltype(&mradm_render_binaural_bank_destroy)> handle_{
        nullptr, &mradm_render_binaural_bank_destroy};
    std::span<const MradmRenderBinauralSourceKey> keys_;
};

// File streaming binaural session over the same prepared HRTF tables + source list as
// render_window. It renders render_block_size-aligned blocks on demand into a FIFO, from
// which process() serves any requested frame count through the same Rust per-source OLA as
// render_window, so a gap-free run from frame 0 is bit-identical to the offline render (the smoke
// test asserts this). saf_spreader mode (STFT latency compensation) is not yet streamable and is
// rejected at open. seek() rebuilds the per-source OLA / diffuse / HRTF-cache state with the
// offline window's aligned pre-roll and current controls. set_overrides drives per-source sample
// gain ramps and coalesced topology state crossfades. The expensive prepared state is borrowed by
// reference; the owning factory keeps it alive for the stream's lifetime.
class BinauralStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<BinauralStream>>
    create(const BinauralPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        if (plan.binaural_spread_mode == BinauralSpreadMode::saf_spreader && prepared.info.groups != 0U) {
            return make_error(ErrorCode::unsupported,
                              "binaural realtime stream does not yet support saf_spreader mode (use cloud / none)",
                              {});
        }
        const auto& info = plan.scene.info;
        if (info.sample_rate != 48000U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                              "input=" + plan.input_path);
        }
        auto reader = audio::RenderInputReader::open(plan.input_path);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        SourceBank bank;
        if (auto result = bank.rebuild(prepared, {}, plan.input_path); !result) {
            return tl::unexpected{result.error()};
        }
        (void) logs;
        return std::unique_ptr<BinauralStream>{new BinauralStream(prepared,
                                                                  std::move(*reader),
                                                                  std::move(bank),
                                                                  plan.input_path,
                                                                  info.num_channels,
                                                                  info.num_frames,
                                                                  info.sample_rate,
                                                                  plan.object_smoothing_frames)};
    }

    ~BinauralStream() override = default;
    BinauralStream(const BinauralStream&) = delete;
    BinauralStream& operator=(const BinauralStream&) = delete;
    BinauralStream(BinauralStream&&) = delete;
    BinauralStream& operator=(BinauralStream&&) = delete;

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (frames_done_ >= total_frames_) {
                    break; // end of material
                }
                if (auto result = render_block(); !result) {
                    return tl::unexpected{result.error()};
                }
                if (fifo_read_ >= fifo_.size()) {
                    break;
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / 2U;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * 2U, out.data() + (produced * 2U));
            fifo_read_ += take * 2U;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override { return seek_with_cancel(frame, {}); }

    [[nodiscard]] bool seek_requires_preroll() const override { return true; }

    [[nodiscard]] Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
        }
        const uint64_t target = std::min(frame, total_frames_);
        const auto start =
            ola_preroll_start(target, render_block_size_, static_cast<uint64_t>(prepared_.bs->overlap_len));
        if (auto result = reader_->seek_frame(start); !result) {
            return tl::unexpected{result.error()};
        }
        frames_done_ = start;
        fifo_.clear();
        old_fifo_.clear();
        fifo_read_ = 0;

        // A discontinuity has no outgoing graph to crossfade. Adopt the latest edits before pre-roll,
        // including targets still waiting for the live topology update interval at the old position.
        if (topology_pending_) {
            if (auto rebuilt = rebuild_sources(pending_topology_); !rebuilt) {
                return rebuilt;
            }
            applied_topology_ = pending_topology_;
            topology_pending_ = false;
        } else {
            bank_.reset();
        }
        next_topology_update_frame_ = target;
        for (auto& slot : live_gain_slots_) {
            slot.gain_ramp().reset();
        }
        update_live_gain_targets();

        while (frames_done_ < target) {
            if (cancel.stop_requested()) {
                return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
            }
            const uint64_t block_start = frames_done_;
            if (auto result = render_block(); !result) {
                return result;
            }
            // Keep the suffix of the containing block so arbitrary pulls never shift the DSP grid.
            fifo_read_ = static_cast<std::size_t>(std::min(target - block_start, frames_done_ - block_start)) * 2U;
        }
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        // Gain targets are resolved per semantic source key. The render block builds one shared
        // sample-domain envelope per key, so every spatial component of an object follows the same
        // de-zippered trajectory (including per-channel DirectSpeakers overrides).
        live_overrides_ = overrides;
        live_override_view_ = live_overrides_;
        update_live_gain_targets();

        // Topology changes need a new source graph and fresh convolution state. Queue the newest
        // graph while dragging; render_block applies at most one every 100 ms and crossfades old
        // and new states over the same source block. This preserves the old OLA/diffuse tail at the
        // boundary and prevents a 60 Hz UI gesture from repeatedly destroying the DSP state.
        std::unordered_map<std::string, LiveTopologyScales> topo;
        for (const auto& ov : overrides.objects) {
            const LiveTopologyScales scales{
                ov.diffuse_scale,
                ov.extent_scale * ov.extent_width_scale,
                ov.extent_scale * ov.extent_height_scale,
                ov.extent_scale * ov.extent_depth_scale,
                ov.divergence_scale,
            };
            if (scales[k_topology_diffuse] != 1.0F || scales[k_topology_width] != 1.0F ||
                scales[k_topology_height] != 1.0F || scales[k_topology_depth] != 1.0F ||
                scales[k_topology_divergence] != 1.0F) {
                topo[ov.object_id] = scales;
            }
        }
        pending_topology_ = std::move(topo);
        topology_pending_ = pending_topology_ != applied_topology_;
    }

    // Live listener head orientation (head tracking / free-look). Applied at the worker render
    // stage: world-locked sources are rotated into the head frame before the HRTF lookup (cheap —
    // it only changes the per-source HRTF direction; no re-prepare). Head-locked sources ignore it.
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        listener_orientation_ = orientation;
    }

    [[nodiscard]] uint32_t out_channels() const override { return 2U; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    BinauralStream(const BinauralPrepared& prepared,
                   std::unique_ptr<audio::RenderInputReader> reader,
                   SourceBank bank,
                   std::string input_path,
                   uint16_t num_in_ch,
                   uint64_t total_frames,
                   uint32_t sample_rate,
                   uint32_t object_smoothing_frames)
        : prepared_(prepared), reader_(std::move(reader)), bank_(std::move(bank)), input_path_(std::move(input_path)),
          num_in_ch_(num_in_ch), total_frames_(total_frames), sample_rate_(sample_rate),
          object_smoothing_frames_(object_smoothing_frames), render_block_size_(prepared.render_block_size),
          in_block_(static_cast<std::size_t>(num_in_ch) * prepared.render_block_size, 0.0F),
          topology_update_interval_frames_(
              std::max<uint64_t>(1U, (static_cast<uint64_t>(sample_rate) * k_live_topology_min_interval_ms) / 1000U)) {
        assign_live_gain_slots();
    }

    class LiveGainSlot {
      public:
        LiveGainSlot(std::string object, std::string speaker_label, uint32_t sample_rate, std::size_t block_frames)
            : object_id(std::move(object)), speaker_label_key(std::move(speaker_label)), ramp(sample_rate),
              envelope(block_frames, 1.0F) {}

        [[nodiscard]] const std::string& object() const noexcept { return object_id; }
        [[nodiscard]] const std::string& speaker_label() const noexcept { return speaker_label_key; }
        [[nodiscard]] render_common::LiveGainRamp& gain_ramp() noexcept { return ramp; }
        [[nodiscard]] std::vector<float>& gain_envelope() noexcept { return envelope; }
        [[nodiscard]] const std::vector<float>& gain_envelope() const noexcept { return envelope; }

      private:
        std::string object_id;
        std::string speaker_label_key;
        render_common::LiveGainRamp ramp;
        std::vector<float> envelope;
    };

    void assign_live_gain_slots() {
        source_gain_slots_.clear();
        source_gain_slots_.reserve(bank_.size());
        for (std::size_t i = 0; i < bank_.size(); ++i) {
            std::string key{bank_.object_id(i)};
            key.push_back('\0');
            key += bank_.label_key(i);
            const auto found = gain_slot_by_key_.find(key);
            if (found != gain_slot_by_key_.end()) {
                source_gain_slots_.push_back(found->second);
                continue;
            }
            const std::size_t slot = live_gain_slots_.size();
            gain_slot_by_key_.emplace(key, slot);
            live_gain_slots_.emplace_back(std::string{bank_.object_id(i)},
                                          std::string{bank_.label_key(i)},
                                          sample_rate_,
                                          static_cast<std::size_t>(render_block_size_));
            source_gain_slots_.push_back(slot);
        }
        update_live_gain_targets();
    }

    void update_live_gain_targets() {
        for (auto& slot : live_gain_slots_) {
            const float target =
                render_common::resolve_live_channel_gain(live_override_view_, slot.object(), slot.speaker_label())
                    .value_or(1.0F);
            slot.gain_ramp().set_target(target);
        }
    }

    void prepare_live_gain_envelopes(std::size_t frames) {
        for (auto& slot : live_gain_slots_) {
            slot.gain_ramp().fill(std::span{slot.gain_envelope()}.first(frames));
        }
    }

    // Cheap re-prepare: the sources of the scene with each overridden object's block diffuse /
    // extent (width,height,depth) / divergence multiplied by its scale (clamped to the same ADM
    // ranges as a semantic policy without max), with fresh per-source state. Empty `topo` rebuilds
    // the unscaled scene bit-for-bit.
    Result<void> rebuild_sources(const std::unordered_map<std::string, LiveTopologyScales>& topo) {
        if (auto result = bank_.rebuild(prepared_, topo, input_path_); !result) {
            return result;
        }
        assign_live_gain_slots();
        return {};
    }

    Result<void> render_current_sources(std::vector<float>& output, uint64_t frames_now) {
        const auto fn = static_cast<std::size_t>(frames_now);
        // Live head tracking: rotate world-locked sources into the head frame at render time (worker
        // stage). Head-locked sources (per-object override) keep their raw direction. Identity pose
        // ⇒ no rotation, output stays bit-identical to no-tracking.
        const bool head_active = !listener_orientation_.is_identity();
        const std::array pose{
            listener_orientation_.yaw_deg, listener_orientation_.pitch_deg, listener_orientation_.roll_deg};
        head_locked_.resize(bank_.size());
        for (std::size_t si = 0; si < bank_.size(); ++si) {
            const auto locked =
                render_common::resolve_live_head_locked(live_override_view_, bank_.object_id(si), bank_.label_key(si));
            head_locked_[si] = locked.has_value() ? static_cast<int32_t>(*locked) : -1;
        }
        const MradmRenderBinauralSourceOutput* outputs = nullptr;
        render_common::DriverError error;
        if (const auto code = mradm_render_binaural_bank_render(bank_.get(),
                                                                in_block_.data(),
                                                                in_block_.size(),
                                                                num_in_ch_,
                                                                frames_done_,
                                                                fn,
                                                                object_smoothing_frames_,
                                                                head_active ? 1U : 0U,
                                                                pose.data(),
                                                                head_locked_.data(),
                                                                head_locked_.size(),
                                                                &outputs,
                                                                error.location(),
                                                                error.message(),
                                                                error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }

        // Reduce per-source L/R into the interleaved FIFO, applying the live per-object
        // gain. With no overrides every gain is 1.0 and the summation order matches
        // render_window's reduce, so the output is bit-identical to the offline render.
        output.assign(fn * 2U, 0.0F);
        const auto block = static_cast<std::size_t>(render_block_size_);
        const std::span sources{outputs, bank_.size()};
        for (std::size_t si = 0; si < sources.size(); ++si) {
            dsp::accumulate_stereo(output,
                                   {sources[si].left, block},
                                   {sources[si].right, block},
                                   live_gain_slots_[source_gain_slots_[si]].gain_envelope(),
                                   fn);
        }
        return {};
    }

    Result<void> render_block() {
        const uint64_t frames_now = std::min<uint64_t>(render_block_size_, total_frames_ - frames_done_);
        const auto fn = static_cast<std::size_t>(frames_now);
        const auto read_result = reader_->read(in_block_.data(), frames_now);
        if (!read_result) {
            return tl::unexpected{read_result.error()};
        }
        if (*read_result != frames_now) {
            return make_error(ErrorCode::io_error, "short render input read");
        }
        // Before the first sample there is no outgoing state to preserve. Apply the coalesced initial
        // topology directly, then begin gain envelopes on the final source graph.
        if (frames_done_ == 0U && topology_pending_) {
            if (auto rebuilt = rebuild_sources(pending_topology_); !rebuilt) {
                return rebuilt;
            }
            applied_topology_ = pending_topology_;
            topology_pending_ = false;
        }
        prepare_live_gain_envelopes(fn);

        const bool apply_topology = topology_pending_ && frames_done_ >= next_topology_update_frame_;
        if (apply_topology) {
            if (auto result = render_current_sources(old_fifo_, frames_now); !result) {
                return result;
            }
            if (auto rebuilt = rebuild_sources(pending_topology_); !rebuilt) {
                return rebuilt;
            }
            if (auto result = render_current_sources(fifo_, frames_now); !result) {
                return result;
            }
            dsp::topology_crossfade(old_fifo_, fifo_, fn);
            applied_topology_ = pending_topology_;
            topology_pending_ = false;
            next_topology_update_frame_ = frames_done_ + topology_update_interval_frames_;
        } else if (auto result = render_current_sources(fifo_, frames_now); !result) {
            return result;
        }
        fifo_read_ = 0;
        frames_done_ += frames_now;
        return {};
    }

    const BinauralPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<audio::RenderInputReader> reader_;
    SourceBank bank_; // own (rebuildable) source list; starts as the prepared list
    std::string input_path_;
    uint16_t num_in_ch_;
    uint64_t total_frames_;
    uint32_t sample_rate_;
    uint32_t object_smoothing_frames_;
    uint64_t render_block_size_;

    std::vector<float> in_block_;
    std::vector<int32_t> head_locked_;
    LiveOverrides live_overrides_; // latest snapshot; gain targets are projected into live_gain_slots_
    render_common::LiveOverrideView live_override_view_{live_overrides_}; // borrows the owned snapshot's labels
    std::unordered_map<std::string, std::size_t> gain_slot_by_key_;
    std::vector<LiveGainSlot> live_gain_slots_;
    std::vector<std::size_t> source_gain_slots_; // source index → live_gain_slots_ index
    ListenerOrientation listener_orientation_{}; // live head pose (worker-only; identity = no tracking)
    // object_id → non-unity topology scales; only non-unity entries.
    // The last applied topology, so set_overrides rebuilds only when it actually changes.
    std::unordered_map<std::string, LiveTopologyScales> applied_topology_;
    std::unordered_map<std::string, LiveTopologyScales> pending_topology_;
    bool topology_pending_{false};
    uint64_t topology_update_interval_frames_{1U};
    uint64_t next_topology_update_frame_{0U};

    std::vector<float> fifo_;     // interleaved L/R produced, not yet served
    std::vector<float> old_fifo_; // one block of the outgoing topology during a state crossfade
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "binaural: open_stream received an incompatible prepared state", {});
        }
        auto stream = BinauralStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport BinauralRenderer::capabilities() const {
    return binaural_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> BinauralRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic) {
        return make_error(ErrorCode::unsupported,
                          "SAF binaural renderer does not support explicit DirectSpeakers routing; use automatic",
                          {});
    }
    auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "binaural");
    if (!lfe_routing) {
        return tl::unexpected{lfe_routing.error()};
    }
    const auto& info = plan.scene.info;

    if (info.sample_rate != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                          "input=" + plan.input_path);
    }

    const uint64_t render_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);

    // One-time setup: load the HRIR dataset, then build the HRTF table and interpolation grid.
    auto state_result = HrtfState::prepare(plan.sofa_path, info.sample_rate, render_block_size);
    if (!state_result) {
        return tl::unexpected{state_result.error()};
    }
    auto bs = std::move(*state_result);
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("HRTF source: {} ({} dirs, {} taps @ {} Hz)",
                         bs->dataset_name,
                         bs->num_dirs,
                         bs->hrir_len,
                         info.sample_rate));
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, "binaural requires the Rust render scene");
    }

    const MradmRenderBinauralRequest request{numerical_spread(plan.binaural_spread_mode), plan.object_smoothing_frames};
    MradmRenderPrepared* raw = nullptr;
    MradmRenderPreparedView view{};
    MradmRenderBinauralInfo plan_info{};
    const auto code =
        mradm_render_binaural_prepare(plan.scene_handle->raw, bs->handle.get(), &request, &raw, &view, &plan_info);
    auto rust = render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "binaural");
    if (!rust) {
        return tl::unexpected{rust.error()};
    }
    auto prepared = std::make_shared<BinauralPrepared>();
    prepared->bs = std::move(bs);
    prepared->rust = std::move(*rust);
    prepared->info = plan_info;
    prepared->render_block_size = render_block_size;
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> BinauralRenderer::render_window(const IPreparedRender& prep,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) {
    const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "binaural: render_window received an incompatible prepared state", {});
    }
    const auto& info = plan.scene.info;
    const uint64_t num_frames = info.num_frames;
    const bool windowed = plan.render_window.has_value();
    const auto& pose = plan.listener_orientation;
    const MradmRenderBinauralOfflineSettings settings{info.num_channels,
                                                      num_frames,
                                                      info.sample_rate,
                                                      windowed ? 1U : 0U,
                                                      windowed ? plan.render_window->start_frame : 0U,
                                                      windowed ? plan.render_window->frame_count : 0U,
                                                      plan.object_smoothing_frames,
                                                      pose.is_identity() ? 0U : 1U,
                                                      {pose.yaw_deg, pose.pitch_deg, pose.roll_deg}};

    // The input, the per-output (primed) spreader adapters, the OLA state and the meter.
    const auto path = audio::wav_path_utf8(plan.input_path);
    MradmRenderBinauralOffline* raw = nullptr;
    MradmAdmText pools{};
    render_common::DriverError error;
    if (const auto code = mradm_render_binaural_offline_open(prepared->rust->handle.get(),
                                                             reinterpret_cast<const uint8_t*>(path.data()),
                                                             path.size(),
                                                             &settings,
                                                             &raw,
                                                             &pools,
                                                             error.location(),
                                                             error.message(),
                                                             error.message_len());
        code != 0) {
        return error.error(code, plan.input_path);
    }
    const std::unique_ptr<MradmRenderBinauralOffline, decltype(&mradm_render_binaural_offline_destroy)> driver(
        raw, &mradm_render_binaural_offline_destroy);
    auto writer_res = audio::WriterHandle::open(plan.output_path, 2U, info.sample_rate, "binaural");
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;
    if (pools.len != 0U) {
        logs.log(LogLevel::info, "binaural", std::string{reinterpret_cast<const char*>(pools.data), pools.len});
    }

    progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.2, 0.0, 0, 0, "rendering"});
    while (const auto pending = mradm_render_binaural_offline_pending(driver.get())) {
        if (pending == 1U && plan.cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
        }
        MradmRenderBinauralStep step{};
        if (const auto code = mradm_render_binaural_offline_next(
                driver.get(), &step, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, plan.input_path);
        }
        if (step.emit != 0U && writer.write(step.output, step.emit) != step.emit) {
            return make_error(ErrorCode::io_error,
                              step.pending == 2U ? "short write during binaural spreader tail"
                                                 : "short write during binaural render",
                              "output=" + plan.output_path);
        }
        if (step.has_progress != 0U) {
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  step.progress.fraction,
                                  step.progress.stage_fraction,
                                  step.progress.done,
                                  step.progress.total,
                                  "rendering"});
        }
    }
    uint64_t out_written = 0;
    MradmRenderOfflineMetrics measured{};
    (void) mradm_render_binaural_offline_finish(driver.get(), &out_written, &measured);

    progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
    const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
    const uint64_t win_end = windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("wrote {} frames to {}{}",
                         out_written,
                         plan.output_path,
                         windowed ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, num_frames)
                                  : std::string{}));

    RenderMetrics metrics;
    if (measured.has_lufs != 0U) {
        metrics.measured_lufs = measured.lufs;
    }
    if (measured.has_peak != 0U) {
        // Rust keeps the binaural backend's established -200 dB silent floor.
        metrics.measured_peak_dbtp = measured.peak_dbtp;
    }
    return metrics;
}

} // namespace
} // namespace binaural_internal

CapabilityReport binaural_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-binaural-hrtf";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.hrtf_sources = {"built-in"};
#if MR_ADM_ENABLE_SOFA
    r.hrtf_sources.emplace_back("user-sofa");
#endif
    // Default (cloud/none) path windows via seek + aligned pre-roll; the experimental
    // saf_spreader path keeps full STFT warm-up and only trims output.
    r.supports_render_window = true;
    r.supported_layouts = {
        // clang-format off
        {"binaural", "SAF HRTF binaural (KEMAR or user SOFA HRIR)", 2, false, 0, true, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

bool binaural_sofa_supported() {
#if MR_ADM_ENABLE_SOFA
    return true;
#else
    return false;
#endif
}

} // namespace mradm
