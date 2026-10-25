#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/render_room.h"

#include "../adm_audio/wav_backend.h"
#include "meter.h"
#include "post_gain.h"
#include "render_common.h"
#include "room_internal.h"
#include "scene_handle.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
constexpr std::size_t k_read_frames = 2048;
using render_common::DriverError;
using render_common::text;

struct FilePrepared final : IPreparedRender {
    std::shared_ptr<render_common::RustPrepared> rust;
    std::shared_ptr<const room_internal::Prepared> room;
    RoomConfig config;
    std::uint64_t frames{0};
    std::uint16_t input_channels{0};
};
std::vector<MradmRenderTripleBalanceOverride> override_records(const LiveOverrides& overrides) {
    std::vector<MradmRenderTripleBalanceOverride> result;
    result.reserve(overrides.objects.size());
    std::ranges::transform(overrides.objects, std::back_inserter(result), [](const LiveObjectOverride& o) {
        return MradmRenderTripleBalanceOverride{{text(o.object_id),
                                                 text(o.speaker_label),
                                                 o.mute ? 1U : 0U,
                                                 o.gain_db,
                                                 o.head_locked.has_value() ? 1U : 0U,
                                                 o.head_locked.value_or(false) ? 1U : 0U},
                                                o.extent_scale,
                                                o.extent_width_scale,
                                                o.extent_height_scale,
                                                o.extent_depth_scale,
                                                o.diffuse_scale,
                                                o.divergence_scale};
    });
    return result;
}
// Reading, the room runtime, events, the tail, seek steps and live edits are Rust mradm-render
// `room_stream` (PCM consistency 4.6 E); this side owns cancellation.
class RoomStream final : public IRenderStream {
  public:
    static Result<std::unique_ptr<IRenderStream>> create(const FilePrepared& prepared, const RenderPlan& plan) {
        if (plan.scene.info.num_frames != prepared.frames || plan.scene.info.num_channels != prepared.input_channels ||
            plan.scene.info.sample_rate != prepared.room->info.sample_rate) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 输入格式不匹配");
        }
        const auto path = audio::wav_path_utf8(plan.input_path);
        const room_internal::ConfigRecord config(prepared.config);
        const std::array<float, 3> pose{
            plan.listener_orientation.yaw_deg, plan.listener_orientation.pitch_deg, plan.listener_orientation.roll_deg};
        DriverError error;
        MradmRenderRoomStream* raw = nullptr;
        if (const auto code = mradm_render_room_stream_open(prepared.rust->handle.get(),
                                                            prepared.room->handle.get(),
                                                            reinterpret_cast<const uint8_t*>(path.data()),
                                                            path.size(),
                                                            &config.record,
                                                            pose.data(),
                                                            &raw,
                                                            error.location(),
                                                            error.message(),
                                                            error.message_len());
            code != 0) {
            return error.error(code, plan.input_path);
        }
        return std::unique_ptr<IRenderStream>{new RoomStream(raw, prepared, plan)};
    }
    Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_render_room_stream_process(handle_.get(),
                                                               out.data(),
                                                               out.size(),
                                                               frames,
                                                               &produced,
                                                               error.location(),
                                                               error.message(),
                                                               error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return produced;
    }
    Result<void> seek(std::uint64_t frame) override { return seek_with_cancel(frame, {}); }
    [[nodiscard]] bool seek_requires_preroll() const override { return true; }
    Result<void> seek_with_cancel(std::uint64_t frame, std::stop_token cancel) override {
        DriverError error;
        std::uint32_t pending = 0;
        if (const auto code = mradm_render_room_stream_seek_begin(
                handle_.get(), frame, &pending, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        while (pending != 0U) {
            if (cancel.stop_requested()) {
                return make_error(ErrorCode::cancelled, "room-binaural 预热已取消");
            }
            if (const auto code = mradm_render_room_stream_seek_step(
                    handle_.get(), &pending, error.location(), error.message(), error.message_len());
                code != 0) {
                return error.error(code, input_path_);
            }
        }
        return {};
    }
    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        const auto records = override_records(overrides);
        std::array<std::uint8_t, 512> message{};
        auto message_len = message.size();
        if (const auto code = mradm_render_room_stream_validate(
                handle_.get(), records.data(), records.size(), message.data(), &message_len);
            code != 0) {
            return make_error(static_cast<ErrorCode>(code),
                              std::string{reinterpret_cast<const char*>(message.data()), message_len});
        }
        return {};
    }
    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = override_records(overrides);
        if (mradm_render_room_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        mradm_render_room_stream_pose(handle_.get(), orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
    }
    [[nodiscard]] std::shared_ptr<IRoomControl> room_control() const override { return control_; }
    [[nodiscard]] std::uint64_t appended_tail_frames() const override {
        return mradm_render_room_stream_tail(handle_.get());
    }
    [[nodiscard]] std::uint32_t out_channels() const override { return 2; }
    [[nodiscard]] std::uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    RoomStream(MradmRenderRoomStream* raw, const FilePrepared& prepared, const RenderPlan& plan)
        : handle_(raw, &mradm_render_room_stream_destroy),
          control_(room_internal::control(mradm_render_room_stream_control(raw))), input_path_(plan.input_path),
          sample_rate_(prepared.room->info.sample_rate) {}
    std::unique_ptr<MradmRenderRoomStream, decltype(&mradm_render_room_stream_destroy)> handle_;
    std::shared_ptr<IRoomControl> control_;
    std::string input_path_;
    std::uint32_t sample_rate_;
};

class RoomRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return room_capabilities(); }
    Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const FilePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 类型错误");
        }
        return RoomStream::create(*prepared, plan);
    }
    Result<RenderMetrics> render_window(const IPreparedRender& state,
                                        const RenderPlan& plan,
                                        ProgressSink& progress,
                                        LogSink& logs) override {
        auto stream = open_stream(state, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        const auto total = plan.scene.info.num_frames + (*stream)->appended_tail_frames();
        const auto start = plan.render_window ? std::min(plan.render_window->start_frame, total) : 0;
        const auto count = plan.render_window ? std::min(plan.render_window->frame_count, total - start) : total;
        auto seek = (*stream)->seek_with_cancel(start, plan.cancel_token);
        if (!seek) {
            return tl::unexpected{seek.error()};
        }
        auto writer = audio::WriterHandle::open(plan.output_path, 2, (*stream)->sample_rate(), "binaural");
        if (!writer) {
            return tl::unexpected{writer.error()};
        }
        auto meter = dsp::Meter::create(2, (*stream)->sample_rate(), dsp::MeterMode::integrated_true_peak);
        if (!meter) {
            return tl::unexpected{meter.error()};
        }
        std::array<float, k_read_frames * 2> output{};
        std::uint64_t written = 0;
        while (written < count) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "room-binaural 离线渲染已取消");
            }
            auto rendered = (*stream)->process(
                output, static_cast<std::size_t>(std::min<std::uint64_t>(k_read_frames, count - written)));
            if (!rendered) {
                return tl::unexpected{rendered.error()};
            }
            if (*rendered == 0 || writer->write(output.data(), *rendered) != *rendered) {
                return make_error(ErrorCode::io_error, "room-binaural 输出未完成");
            }
            const auto measured = render_common::meter_window_chunk(plan.meter_window, start + written, *rendered);
            if (measured.frame_count != 0) {
                auto metered = meter->add_frames(output.data() + (2 * measured.offset_frames), measured.frame_count);
                if (!metered) {
                    return tl::unexpected{metered.error()};
                }
            }
            written += *rendered;
            const auto fraction = static_cast<double>(written) / static_cast<double>(count);
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  fraction,
                                  fraction,
                                  written,
                                  count,
                                  "room-binaural"});
        }
        auto finish = writer->finish();
        if (!finish) {
            return tl::unexpected{finish.error()};
        }
        RenderMetrics metrics;
        if (auto value = meter->integrated(); value && std::isfinite(*value)) {
            metrics.measured_lufs = *value;
        }
        if (auto value = meter->max_true_peak(); value) {
            metrics.measured_peak_dbtp = dsp::amplitude_to_db(std::max(*value, 1e-10));
        }
        return metrics;
    }
};

// Sources and events are Rust mradm-render `room` (PCM consistency 4.6); this shell validates the
// screens and prepares the room resources for the file stream.
Result<std::shared_ptr<IPreparedRender>> RoomRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    auto screen_valid = validate_screen_config(plan.screens);
    if (!screen_valid) {
        return tl::unexpected{screen_valid.error()};
    }
    auto room = room_internal::prepare(plan.room, plan.sofa_path.value_or(std::filesystem::path{}));
    if (!room) {
        return tl::unexpected{room.error()};
    }
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, "room-binaural requires the Rust render scene");
    }
    const auto& screens = plan.screens;
    const MradmRenderRoomRequest request{render_common::text(plan.output_layout),
                                         (*room)->info.sample_rate,
                                         static_cast<std::uint32_t>(plan.binaural_spread_mode),
                                         static_cast<std::uint32_t>(plan.direct_speakers_routing_mode),
                                         plan.direct_speakers_matrix ? 1U : 0U,
                                         plan.default_interp_ms,
                                         screens.reference ? 1U : 0U,
                                         room_internal::geometry(screens.reference.value_or(ScreenGeometry{})),
                                         screens.reproduction ? 1U : 0U,
                                         room_internal::geometry(screens.reproduction.value_or(ScreenGeometry{}))};
    MradmRenderPrepared* raw = nullptr;
    MradmRenderPreparedView view{};
    const auto code = mradm_render_room_prepare(plan.scene_handle->raw, &request, &raw, &view);
    auto rust =
        render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "room-binaural");
    if (!rust) {
        return tl::unexpected{rust.error()};
    }
    auto result = std::make_shared<FilePrepared>();
    result->rust = std::move(*rust);
    result->room = *room;
    result->config = plan.room;
    result->frames = plan.scene.info.num_frames;
    result->input_channels = plan.scene.info.num_channels;
    return std::shared_ptr<IPreparedRender>{std::move(result)};
}
} // namespace
CapabilityReport room_capabilities() {
    CapabilityReport result;
    result.backend_name = "room-binaural";
    result.backend_version = "1";
    result.supports_objects = true;
    result.supports_direct_speakers = true;
    result.supports_channel_lock = true;
    result.supports_object_divergence = true;
    result.supports_diffuse = true;
    result.supports_screen_ref = true;
    result.supports_render_window = true;
    result.hrtf_sources = {"user-sofa", "prepared-hrir", "hrtf-collection"};
    result.supported_layouts = {{"binaural", "Room binaural", 2, true, 0, true, true}};
    result.modes = {{"hybrid-shoebox-v1", "Finite hybrid room", {"binaural"}, {48000, 96000}, true}};
    result.qualities = {{"exact", "精确", {"binaural"}, {48000, 96000}, true},
                        {"realtime", "实时", {"binaural"}, {48000, 96000}, true}};
    return result;
}
std::unique_ptr<IRenderer> create_room_renderer() {
    return std::make_unique<RoomRenderer>();
}
} // namespace mradm
