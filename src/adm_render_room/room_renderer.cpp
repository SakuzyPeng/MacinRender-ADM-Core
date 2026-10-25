#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include "adm/audio_io.h"
#include "adm/render_room.h"

#include "meter.h"
#include "post_gain.h"
#include "render_common.h"
#include "room_internal.h"
#include "scene_handle.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
constexpr std::size_t k_read_frames = 2048;
struct FileData {
    std::shared_ptr<const room_internal::Prepared> room;
    RoomConfig config;
    std::vector<MradmRoomDescription> descriptions;
    std::vector<std::uint16_t> channels;
    std::vector<std::string> objects;
    std::vector<std::string> labels;
    // The Rust event table (mradm-render `room`), sorted by sample, priority and source.
    std::vector<MradmRenderRoomEvent> events;
    std::uint64_t frames{0};
    std::uint16_t input_channels{0};
    bool spread{true};
};
struct FilePrepared final : IPreparedRender {
    std::shared_ptr<const FileData> data;
};
MradmRoomOverride live_value(const LiveObjectOverride* value) {
    MradmRoomOverride result{0, 0, {1, 1, 1}, 1, 1, 0, 0, 0};
    if (value != nullptr) {
        result.gain_db = value->gain_db;
        result.mute = value->mute ? 1U : 0U;
        result.extent[0] = value->extent_scale * value->extent_width_scale;
        result.extent[1] = value->extent_scale * value->extent_height_scale;
        result.extent[2] = value->extent_scale * value->extent_depth_scale;
        result.diffuse = value->diffuse_scale;
        result.divergence = value->divergence_scale;
        result.has_head = value->head_locked.has_value() ? 1U : 0U;
        result.head = value->head_locked.value_or(false) ? 1U : 0U;
    }
    return result;
}
class RoomStream final : public IRenderStream {
  public:
    static Result<std::unique_ptr<IRenderStream>> create(std::shared_ptr<const FileData> data, const RenderPlan& plan) {
        if (plan.scene.info.num_frames != data->frames || plan.scene.info.num_channels != data->input_channels ||
            plan.scene.info.sample_rate != data->room->info.sample_rate) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 输入格式不匹配");
        }
        auto reader = audio::RenderInputReader::open(plan.input_path);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        auto runtime = room_internal::Runtime::create(data->room, data->config, false, data->spread);
        if (!runtime) {
            return tl::unexpected{runtime.error()};
        }
        auto configured = (*runtime)->configure(data->descriptions);
        if (!configured) {
            return tl::unexpected{configured.error()};
        }
        auto pose = (*runtime)->pose(plan.listener_orientation);
        if (!pose) {
            return tl::unexpected{pose.error()};
        }
        auto stream = std::unique_ptr<RoomStream>{new RoomStream(std::move(data), std::move(*reader), *runtime)};
        auto reset = stream->seek(0);
        if (!reset) {
            return tl::unexpected{reset.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(stream)};
    }
    Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        if (frames > out.size() / 2) {
            return make_error(ErrorCode::invalid_argument, "room-binaural 输出缓冲区不足");
        }
        if (error_) {
            return tl::unexpected{*error_};
        }
        std::size_t produced = 0;
        while (produced < frames) {
            if (position_ == data_->frames && !ended_) {
                auto tail = runtime_->end_input();
                if (!tail) {
                    return tl::unexpected{tail.error()};
                }
                ended_ = true;
                end_ = data_->frames + *tail;
            }
            if (ended_ && position_ == end_) {
                break;
            }
            auto take = std::min(k_read_frames, frames - produced);
            take = static_cast<std::size_t>(std::min<std::uint64_t>(take, (ended_ ? end_ : data_->frames) - position_));
            std::ranges::fill(planes_, MradmRoomPlane{});
            events_.clear();
            if (!ended_) {
                auto read = reader_->read(input_.data(), take);
                if (!read) {
                    return tl::unexpected{read.error()};
                }
                if (*read != take) {
                    return make_error(ErrorCode::io_error, "room-binaural 读取素材提前结束");
                }
                for (std::size_t source = 0; source < planes_.size(); ++source) {
                    auto* channel = planar_.data() + (source * k_read_frames);
                    for (std::size_t frame = 0; frame < take; ++frame) {
                        channel[frame] = input_[(frame * data_->input_channels) + data_->channels[source]];
                    }
                    planes_[source] = {channel, take, 1, 0};
                }
                while (event_ < data_->events.size() && data_->events[event_].sample < position_ + take) {
                    auto command = data_->events[event_].command;
                    command.offset = static_cast<std::uint32_t>(data_->events[event_].sample - position_);
                    events_.push_back(command);
                    ++event_;
                }
            }
            auto result = runtime_->process(take, planes_, {}, events_, out.subspan(produced * 2, take * 2));
            if (!result) {
                return tl::unexpected{result.error()};
            }
            position_ += take;
            produced += take;
        }
        return produced;
    }
    Result<void> seek(std::uint64_t frame) override { return seek_with_cancel(frame, {}); }
    [[nodiscard]] bool seek_requires_preroll() const override { return true; }
    Result<void> seek_with_cancel(std::uint64_t frame, std::stop_token cancel) override {
        const auto maximum = data_->frames + runtime_->tail_frames();
        if (frame > maximum) {
            return make_error(ErrorCode::invalid_argument, "room-binaural seek 超过节目及尾声长度");
        }
        const auto history = runtime_->preroll_frames();
        const auto start = frame > history ? ((frame - history) / k_read_frames) * k_read_frames : 0;
        auto reset = runtime_->reset(start);
        if (!reset) {
            return reset;
        }
        // Replay anchors, including independently running ramps, without replaying
        // old PCM. Then reconstruct only the finite acoustic/FFT history.
        event_ = 0;
        while (event_ < data_->events.size() && data_->events[event_].sample < start) {
            auto prime = runtime_->prime(data_->events[event_].command, data_->events[event_].sample);
            if (!prime) {
                return prime;
            }
            ++event_;
        }
        auto positioned = reader_->seek_frame(std::min(start, data_->frames));
        if (!positioned) {
            return positioned;
        }
        position_ = start;
        ended_ = false;
        end_ = maximum;
        error_.reset();
        while (position_ < frame) {
            if (cancel.stop_requested()) {
                return make_error(ErrorCode::cancelled, "room-binaural 预热已取消");
            }
            auto count =
                process(scratch_, static_cast<std::size_t>(std::min<std::uint64_t>(k_read_frames, frame - position_)));
            if (!count) {
                return tl::unexpected{count.error()};
            }
            if (*count == 0) {
                return make_error(ErrorCode::internal_error, "room-binaural 预热提前结束");
            }
        }
        return {};
    }
    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        for (const auto& item : overrides.objects) {
            if (!std::isfinite(item.extent_scale) || !std::isfinite(item.extent_width_scale) ||
                !std::isfinite(item.extent_height_scale) || !std::isfinite(item.extent_depth_scale) ||
                item.extent_scale < 0 || item.extent_width_scale < 0 || item.extent_height_scale < 0 ||
                item.extent_depth_scale < 0) {
                return make_error(ErrorCode::invalid_argument, "房间 extent 比例必须有限且非负");
            }
            const auto value = live_value(&item);
            std::array<char, 512> message{};
            const auto code = mradm_room_validate_overrides(&value, 1, message.data(), message.size());
            if (code != 0) {
                return make_error(static_cast<ErrorCode>(code), message.data());
            }
        }
        return {};
    }
    void set_overrides(const LiveOverrides& overrides) override {
        auto valid = validate_overrides(overrides);
        if (!valid) {
            error_ = valid.error();
            return;
        }
        for (std::size_t source = 0; source < overrides_.size(); ++source) {
            const LiveObjectOverride* whole = nullptr;
            const LiveObjectOverride* specific = nullptr;
            for (const auto& item : overrides.objects) {
                if (item.object_id != data_->objects[source]) {
                    continue;
                }
                if (item.speaker_label.empty()) {
                    whole = &item;
                } else if (!data_->labels[source].empty() &&
                           render_common::canonicalise_speaker_label(item.speaker_label) == data_->labels[source]) {
                    specific = &item;
                }
            }
            auto value = live_value(specific != nullptr ? specific : whole);
            const auto head =
                render_common::resolve_live_head_locked(overrides, data_->objects[source], data_->labels[source]);
            value.has_head = head.has_value() ? 1U : 0U;
            value.head = head.value_or(false) ? 1U : 0U;
            overrides_[source] = value;
        }
        auto applied = runtime_->overrides(overrides_);
        if (!applied) {
            error_ = applied.error();
        }
    }
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        const auto result = runtime_->pose(orientation);
        if (!result) {
            error_ = result.error();
        }
    }
    [[nodiscard]] std::shared_ptr<IRoomControl> room_control() const override { return runtime_; }
    [[nodiscard]] std::uint64_t appended_tail_frames() const override { return runtime_->tail_frames(); }
    [[nodiscard]] std::uint32_t out_channels() const override { return 2; }
    [[nodiscard]] std::uint32_t sample_rate() const override { return data_->room->info.sample_rate; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    RoomStream(std::shared_ptr<const FileData> data,
               std::unique_ptr<audio::RenderInputReader> reader,
               std::shared_ptr<room_internal::Runtime> runtime)
        : data_(std::move(data)), reader_(std::move(reader)), runtime_(std::move(runtime)),
          input_(data_->input_channels * k_read_frames), planar_(data_->channels.size() * k_read_frames),
          planes_(data_->channels.size()), overrides_(data_->channels.size(), live_value(nullptr)) {
        events_.reserve(4096);
    }
    std::shared_ptr<const FileData> data_;
    std::unique_ptr<audio::RenderInputReader> reader_;
    std::shared_ptr<room_internal::Runtime> runtime_;
    std::vector<float> input_;
    std::vector<float> planar_;
    std::vector<MradmRoomPlane> planes_;
    std::vector<MradmRoomCommand> events_;
    std::vector<MradmRoomOverride> overrides_;
    std::array<float, k_read_frames * 2> scratch_{};
    std::optional<Error> error_;
    std::uint64_t position_{0};
    std::uint64_t end_{0};
    std::size_t event_{0};
    bool ended_{false};
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
        return RoomStream::create(prepared->data, plan);
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
// screens, prepares the room resources and keeps the table for the file stream.
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
    MradmRenderRoomView table{};
    const auto code = mradm_render_room_prepare(plan.scene_handle->raw, &request, &raw, &view, &table);
    auto rust =
        render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "room-binaural");
    if (!rust) {
        return tl::unexpected{rust.error()};
    }
    const auto string_of = [](MradmAdmText t) { return std::string{reinterpret_cast<const char*>(t.data), t.len}; };
    auto data = std::make_shared<FileData>();
    data->room = *room;
    data->config = plan.room;
    data->frames = table.frames;
    data->input_channels = table.input_channels;
    data->spread = table.spread != 0;
    data->descriptions.assign(table.descriptions, table.descriptions + table.sources);
    data->channels.assign(table.channels, table.channels + table.sources);
    std::ranges::transform(std::span{table.objects, table.sources}, std::back_inserter(data->objects), string_of);
    std::ranges::transform(std::span{table.labels, table.sources}, std::back_inserter(data->labels), string_of);
    data->events.assign(table.events, table.events + table.events_len);
    auto result = std::make_shared<FilePrepared>();
    result->data = std::move(data);
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
