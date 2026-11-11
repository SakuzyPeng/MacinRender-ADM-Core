#include "speaker_pcm.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

#include "adm/audio_io.h"

#include "render_common.h"
#include "scene_handle.h"
#include "wav_backend.h"

namespace mradm::render_common {
tl::unexpected<Error> DriverError::error(int32_t code, const std::string& input_path) const {
    std::string context;
    if (location_ == 1U) {
        context = "path=" + input_path;
    } else if (location_ == 2U) {
        context = "input=" + input_path;
    } else if (location_ == 3U) {
        context = "meter";
    } else if (location_ == 4U) {
        context = "HOA DSP";
    } else if (location_ == 5U) {
        context = "Triple Balance DSP";
    } else if (location_ == 6U) {
        context = "room-binaural";
    }
    return make_error(static_cast<ErrorCode>(code),
                      std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                      std::move(context));
}

namespace {

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

// The file stream of every Rust backend (mradm-engine `stream`, PCM consistency 5.3 A): the open
// checks, seek steps, their cancellation points and texts, live edits and validation run in Rust;
// this side samples the stop token before every seek call and maps the error locators.
class EngineStream final : public IRenderStream {
  public:
    EngineStream(MradmEngineStream* raw,
                 const MradmEngineStreamInfo& info,
                 std::string input_path,
                 std::string_view output_layout,
                 std::shared_ptr<IRoomControl> control)
        : handle_(raw, &mradm_engine_stream_destroy), control_(std::move(control)), input_path_(std::move(input_path)),
          output_layout_(info.binaural != 0U ? std::string_view{"binaural"} : output_layout),
          outputs_(static_cast<uint32_t>(info.outputs)), sample_rate_(info.sample_rate),
          requires_preroll_(info.requires_preroll != 0U) {}

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_engine_stream_process(handle_.get(),
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

    [[nodiscard]] Result<void> seek(uint64_t frame) override { return seek_with_cancel(frame, {}); }
    [[nodiscard]] bool seek_requires_preroll() const override { return requires_preroll_; }

    [[nodiscard]] Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        DriverError error;
        uint32_t pending = 0;
        if (const auto code = mradm_engine_stream_seek_begin(handle_.get(),
                                                             frame,
                                                             cancel.stop_requested() ? 1U : 0U,
                                                             &pending,
                                                             error.location(),
                                                             error.message(),
                                                             error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        while (pending != 0U) {
            if (const auto code = mradm_engine_stream_seek_step(handle_.get(),
                                                                cancel.stop_requested() ? 1U : 0U,
                                                                &pending,
                                                                error.location(),
                                                                error.message(),
                                                                error.message_len());
                code != 0) {
                return error.error(code, input_path_);
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        const auto records = live_override_records(overrides);
        std::string message(512, '\0');
        std::string context(512, '\0');
        for (const auto& item : overrides.objects) {
            context.resize(std::max(context.size(), item.object_id.size() + 1));
        }
        auto message_len = message.size();
        auto context_len = context.size();
        if (const auto code = mradm_engine_stream_validate(handle_.get(),
                                                           records.data(),
                                                           records.size(),
                                                           reinterpret_cast<uint8_t*>(message.data()),
                                                           &message_len,
                                                           reinterpret_cast<uint8_t*>(context.data()),
                                                           &context_len);
            code != 0) {
            message.resize(message_len);
            context.resize(context_len);
            return make_error(static_cast<ErrorCode>(code), std::move(message), std::move(context));
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = live_override_records(overrides);
        if (mradm_engine_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }

    void set_listener_orientation(const ListenerOrientation& orientation) override {
        mradm_engine_stream_pose(handle_.get(), orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
    }

    [[nodiscard]] std::shared_ptr<IRoomControl> room_control() const override { return control_; }
    [[nodiscard]] std::uint64_t appended_tail_frames() const override {
        return mradm_engine_stream_tail(handle_.get());
    }
    [[nodiscard]] uint32_t out_channels() const override { return outputs_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

    [[nodiscard]] MradmEngineStream* release(MradmEngineStreamInfo& info) {
        info = MradmEngineStreamInfo{
            outputs_, sample_rate_, output_layout_ == "binaural" ? 1U : 0U, requires_preroll_ ? 1U : 0U};
        return handle_.release();
    }

  private:
    std::unique_ptr<MradmEngineStream, decltype(&mradm_engine_stream_destroy)> handle_;
    std::shared_ptr<IRoomControl> control_;
    std::string input_path_;
    std::string output_layout_;
    uint32_t outputs_;
    uint32_t sample_rate_;
    bool requires_preroll_;
};

} // namespace

Result<std::unique_ptr<IRenderStream>>
open_engine_stream(const EnginePrepared& prepared, const RenderPlan& plan, RoomControlAdopter room_control) {
    const auto& info = plan.scene.info;
    const auto& pose = plan.listener_orientation;
    const MradmEngineStreamSettings settings{info.num_channels,
                                             info.num_frames,
                                             info.sample_rate,
                                             plan.default_interp_ms,
                                             plan.object_smoothing_frames,
                                             numerical_spread(plan.binaural_spread_mode),
                                             {pose.yaw_deg, pose.pitch_deg, pose.roll_deg}};
    const auto path = audio::wav_path_utf8(plan.input_path);
    DriverError error;
    MradmEngineStream* raw = nullptr;
    MradmEngineStreamInfo stream_info{};
    if (const auto code = mradm_engine_stream_open(prepared.handle.get(),
                                                   reinterpret_cast<const uint8_t*>(path.data()),
                                                   path.size(),
                                                   &settings,
                                                   &raw,
                                                   &stream_info,
                                                   error.location(),
                                                   error.message(),
                                                   error.message_len());
        code != 0) {
        return error.error(code, plan.input_path);
    }
    return adopt_engine_stream(raw, stream_info, plan.input_path, plan.output_layout, room_control);
}

MradmEngineStream* release_engine_stream(std::unique_ptr<IRenderStream>& stream, MradmEngineStreamInfo& info) {
    auto* engine = dynamic_cast<EngineStream*>(stream.get());
    if (engine == nullptr) {
        return nullptr;
    }
    auto* raw = engine->release(info);
    stream.reset();
    return raw;
}

std::unique_ptr<IRenderStream> adopt_engine_stream(MradmEngineStream* raw,
                                                   const MradmEngineStreamInfo& info,
                                                   const std::string& input_path,
                                                   std::string_view output_layout,
                                                   RoomControlAdopter room_control) {
    std::shared_ptr<IRoomControl> control;
    if (room_control != nullptr) {
        if (auto* handle = mradm_engine_stream_control(raw); handle != nullptr) {
            control = room_control(handle);
        }
    }
    return std::make_unique<EngineStream>(raw, info, input_path, output_layout, std::move(control));
}

namespace {

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string string_of(MradmAdmText t) {
    return {reinterpret_cast<const char*>(t.data), t.len};
}

[[nodiscard]] std::string_view module_of(EngineBackend backend) {
    switch (backend) {
    case EngineBackend::ear:
        return "ear";
    case EngineBackend::saf:
        return "saf-vbap";
    case EngineBackend::triple_balance:
        return "triple-balance";
    case EngineBackend::hoa:
        return "hoa-encode";
    case EngineBackend::binaural:
        return "binaural";
    case EngineBackend::room:
        return "room-binaural";
    }
    return "engine";
}

} // namespace

Result<std::shared_ptr<EnginePrepared>>
prepare_backend(EngineBackend backend, const RenderPlan& plan, LogSink& logs, const void* room_profile) {
    const auto module = module_of(backend);
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, std::string{module} + " requires the Rust render scene");
    }
    static const DirectSpeakersMatrix no_matrix;
    const auto* matrix = plan.direct_speakers_matrix.get();
    const MatrixRecords routes(matrix != nullptr ? *matrix : no_matrix); // borrows the labels
    const auto sofa = plan.sofa_path ? utf8(*plan.sofa_path) : std::string{};
    const auto sofa_name = plan.sofa_path ? utf8(plan.sofa_path->filename()) : std::string{};
    const auto hrir = utf8(plan.room.hrir_manifest);
    const auto& screens = plan.screens;
    const MradmEnginePrepareRequest request{static_cast<uint32_t>(backend),
                                            text(plan.output_layout),
                                            plan.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                                            static_cast<uint32_t>(plan.direct_speakers_routing_mode),
                                            matrix != nullptr ? 1U : 0U,
                                            routes.routes.data(),
                                            routes.routes.size(),
                                            static_cast<uint32_t>(plan.speaker_spread_mode),
                                            static_cast<uint32_t>(plan.binaural_spread_mode),
                                            plan.triple_balance_mode == TripleBalanceMode::d ? 1U : 0U,
                                            plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                            plan.object_smoothing_frames,
                                            plan.default_interp_ms,
                                            text(plan.input_path),
                                            plan.sofa_path ? 1U : 0U,
                                            text(sofa),
                                            text(sofa_name),
                                            screens.reference ? 1U : 0U,
                                            screen_geometry(screens.reference.value_or(ScreenGeometry{})),
                                            screens.reproduction ? 1U : 0U,
                                            screen_geometry(screens.reproduction.value_or(ScreenGeometry{})),
                                            room_profile,
                                            text(hrir),
                                            plan.room.metres_per_unit,
                                            static_cast<uint32_t>(plan.room.quality)};
    MradmRenderPrepared* raw = nullptr;
    MradmEnginePreparedView view{};
    const auto code = mradm_engine_prepare(plan.scene_handle->raw, &request, &raw, &view);
    auto prepared = std::make_shared<EnginePrepared>();
    prepared->handle.reset(raw);
    if (raw == nullptr) {
        return make_error(static_cast<ErrorCode>(code), std::string{module} + " preparation failed");
    }
    // The backend report also describes a rejected preparation.
    if (view.has_report != 0U && plan.renderer_semantics_sink) {
        plan.renderer_semantics_sink(string_of(view.report));
    }
    for (const auto& line : std::span{view.status.logs, view.status.logs_len}) {
        logs.log(static_cast<LogLevel>(line.level), string_of(line.module), string_of(line.message));
    }
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), string_of(view.status.message), string_of(view.status.context));
    }
    return prepared;
}

Result<RenderMetrics>
render_offline(const IPreparedRender& prep, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "offline render received an incompatible prepared state");
    }
    const auto& info = plan.scene.info;
    const auto input = audio::wav_path_utf8(plan.input_path);
    const auto output = audio::wav_path_utf8(plan.output_path);
    const auto& window = plan.render_window;
    const auto& meter = plan.meter_window;
    const auto& pose = plan.listener_orientation;
    const MradmEngineOfflineSettings settings{text(input),
                                              text(output),
                                              text(input),
                                              text(output),
                                              text(plan.output_layout),
                                              info.num_channels,
                                              info.num_frames,
                                              info.sample_rate,
                                              plan.default_interp_ms,
                                              plan.object_smoothing_frames,
                                              window ? 1U : 0U,
                                              window ? window->start_frame : 0,
                                              window ? window->frame_count : 0,
                                              meter ? 1U : 0U,
                                              meter ? meter->start_frame : 0,
                                              meter ? meter->frame_count : 0,
                                              {pose.yaw_deg, pose.pitch_deg, pose.roll_deg}};
    MradmEngineOffline* raw = nullptr;
    if (const auto code = mradm_engine_offline_open(prepared->handle.get(), &settings, &raw); code != 0) {
        return make_error(static_cast<ErrorCode>(code), "invalid offline render arguments");
    }
    const std::unique_ptr<MradmEngineOffline, decltype(&mradm_engine_offline_destroy)> run(
        raw, &mradm_engine_offline_destroy);
    for (;;) {
        MradmEngineOfflineEvent event{};
        MradmEngineStatus status{};
        if (const auto code =
                mradm_engine_offline_next(run.get(), plan.cancel_token.stop_requested() ? 1U : 0U, &event, &status);
            code != 0) {
            return make_error(static_cast<ErrorCode>(code), string_of(status.message), string_of(status.context));
        }
        if (event.kind == 1U) {
            progress.on_progress({static_cast<RenderStage>(event.stage),
                                  static_cast<RenderOperation>(event.operation),
                                  event.fraction,
                                  event.stage_fraction,
                                  event.current,
                                  event.total,
                                  string_of(event.message)});
        } else if (event.kind == 2U) {
            logs.log(static_cast<LogLevel>(event.level), string_of(event.module), string_of(event.message));
        } else if (event.kind == 3U) {
            RenderMetrics metrics;
            if (event.has_lufs != 0U) {
                metrics.measured_lufs = event.lufs;
            }
            if (event.has_peak != 0U) {
                metrics.measured_peak_dbtp = event.peak_dbtp;
            }
            return metrics;
        }
    }
}

} // namespace mradm::render_common
