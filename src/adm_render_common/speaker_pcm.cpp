#include "speaker_pcm.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>

#include <fmt/format.h>

#include "adm/audio_io.h"

#include "../adm_audio/wav_backend.h"
#include "render_common.h"
#include "scene_handle.h"

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

std::vector<MradmRenderOverride> override_records(const LiveOverrides& overrides) {
    std::vector<MradmRenderOverride> result;
    result.reserve(overrides.objects.size());
    std::ranges::transform(overrides.objects, std::back_inserter(result), [](const LiveObjectOverride& o) {
        return MradmRenderOverride{text(o.object_id),
                                   text(o.speaker_label),
                                   o.mute ? 1U : 0U,
                                   o.gain_db,
                                   o.head_locked.has_value() ? 1U : 0U,
                                   o.head_locked.value_or(false) ? 1U : 0U};
    });
    return result;
}

std::vector<MradmRenderLiveOverride> live_override_records(const LiveOverrides& overrides) {
    const auto base = override_records(overrides);
    std::vector<MradmRenderLiveOverride> result;
    result.reserve(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        const auto& o = overrides.objects[i];
        result.push_back({base[i],
                          o.extent_scale,
                          o.extent_width_scale,
                          o.extent_height_scale,
                          o.extent_depth_scale,
                          o.diffuse_scale,
                          o.divergence_scale});
    }
    return result;
}

MatrixRecords::MatrixRecords(const DirectSpeakersMatrix& matrix) {
    targets.reserve(matrix.routes.size());
    for (const auto& route : matrix.routes) {
        auto& records = targets.emplace_back();
        for (const auto& target : route.targets) {
            records.push_back({text(target.label), target.gain});
            label_bytes += target.label.size();
        }
        routes.push_back({text(route.source_label), route.mute ? 1U : 0U, records.data(), records.size()});
        label_bytes += route.source_label.size();
    }
}

namespace {

class FileStream final : public IRenderStream {
  public:
    FileStream(MradmRenderStream* raw, const RenderPlan& plan, uint16_t outputs)
        : handle_(raw, &mradm_render_stream_destroy), input_path_(plan.input_path), outputs_(outputs),
          sample_rate_(plan.scene.info.sample_rate), output_layout_(plan.output_layout) {}

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_render_stream_process(handle_.get(),
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

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        DriverError error;
        if (const auto code =
                mradm_render_stream_seek(handle_.get(), frame, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = override_records(overrides);
        if (mradm_render_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] uint32_t out_channels() const override { return outputs_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

  private:
    std::unique_ptr<MradmRenderStream, decltype(&mradm_render_stream_destroy)> handle_;
    std::string input_path_;
    uint16_t outputs_;
    uint32_t sample_rate_;
    std::string output_layout_;
};

} // namespace

Result<std::unique_ptr<IRenderStream>> open_file_stream(const EnginePrepared& prepared, const RenderPlan& plan) {
    const auto path = audio::wav_path_utf8(plan.input_path);
    DriverError error;
    MradmRenderStream* raw = nullptr;
    if (const auto code = mradm_render_stream_open(prepared.handle.get(),
                                                   reinterpret_cast<const uint8_t*>(path.data()),
                                                   path.size(),
                                                   plan.default_interp_ms,
                                                   plan.object_smoothing_frames,
                                                   &raw,
                                                   error.location(),
                                                   error.message(),
                                                   error.message_len());
        code != 0) {
        return error.error(code, plan.input_path);
    }
    return std::unique_ptr<IRenderStream>{std::make_unique<FileStream>(raw, plan, prepared.outputs)};
}

namespace {

[[nodiscard]] std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string string_of(MradmAdmText t) {
    return {reinterpret_cast<const char*>(t.data), t.len};
}

[[nodiscard]] MradmRoomScreenGeometry geometry(const ScreenGeometry& screen) {
    MradmRoomScreenGeometry out{};
    out.cartesian = screen.cartesian ? 1U : 0U;
    out.aspect_ratio = screen.aspect_ratio;
    std::ranges::copy(screen.centre, std::begin(out.centre));
    out.width = screen.width;
    return out;
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
        return make_error(ErrorCode::internal_error, fmt::format("{} requires the Rust render scene", module));
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
                                            geometry(screens.reference.value_or(ScreenGeometry{})),
                                            screens.reproduction ? 1U : 0U,
                                            geometry(screens.reproduction.value_or(ScreenGeometry{})),
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
        return make_error(static_cast<ErrorCode>(code), fmt::format("{} preparation failed", module));
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
    prepared->outputs = static_cast<uint16_t>(view.outputs);
    prepared->inputs = static_cast<uint16_t>(view.inputs);
    prepared->sample_rate = view.sample_rate;
    prepared->frames = plan.scene.info.num_frames;
    prepared->mode = plan.triple_balance_mode;
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
                                              text(plan.input_path),
                                              text(plan.output_path),
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
