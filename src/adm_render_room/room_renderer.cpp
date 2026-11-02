#include <array>
#include <exception>
#include <span>
#include <string>

#include "adm/render_room.h"

#include "../adm_audio/wav_backend.h"
#include "capability_records.h"
#include "render_common.h"
#include "room_internal.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
using render_common::DriverError;
using render_common::EnginePrepared;

// Reading, the room runtime, events, the tail, seek steps and live edits are Rust mradm-render
// `room_stream` (PCM consistency 4.6 E); this side owns cancellation.
class RoomStream final : public IRenderStream {
  public:
    static Result<std::unique_ptr<IRenderStream>> create(const EnginePrepared& prepared, const RenderPlan& plan) {
        if (plan.scene.info.num_frames != prepared.frames || plan.scene.info.num_channels != prepared.inputs ||
            plan.scene.info.sample_rate != prepared.sample_rate) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 输入格式不匹配");
        }
        const auto path = audio::wav_path_utf8(plan.input_path);
        const std::array<float, 3> pose{
            plan.listener_orientation.yaw_deg, plan.listener_orientation.pitch_deg, plan.listener_orientation.roll_deg};
        DriverError error;
        MradmRenderRoomStream* raw = nullptr;
        if (const auto code = mradm_render_room_stream_open(prepared.handle.get(),
                                                            reinterpret_cast<const uint8_t*>(path.data()),
                                                            path.size(),
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
        const auto records = render_common::live_override_records(overrides);
        std::array<std::uint8_t, 512> message{};
        auto message_len = message.size();
        if (const auto code =
                mradm_render_room_validate_overrides(records.data(), records.size(), message.data(), &message_len);
            code != 0) {
            return make_error(static_cast<ErrorCode>(code),
                              std::string{reinterpret_cast<const char*>(message.data()), message_len});
        }
        return {};
    }
    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = render_common::live_override_records(overrides);
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
    RoomStream(MradmRenderRoomStream* raw, const EnginePrepared& prepared, const RenderPlan& plan)
        : handle_(raw, &mradm_render_room_stream_destroy),
          control_(room_internal::control(mradm_render_room_stream_control(raw))), input_path_(plan.input_path),
          sample_rate_(prepared.sample_rate) {}
    std::unique_ptr<MradmRenderRoomStream, decltype(&mradm_render_room_stream_destroy)> handle_;
    std::shared_ptr<IRoomControl> control_;
    std::string input_path_;
    std::uint32_t sample_rate_;
};

// Sources, events and the room resources are prepared in Rust (mradm-engine `backend`,
// mradm-render `room`, PCM consistency 4.6, 5.2 C1); offline windows render in the engine over the
// file stream.
class RoomRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return room_capabilities(); }
    Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        const void* profile = plan.room.profile ? room_internal::Access::get(*plan.room.profile) : nullptr;
        auto prepared = render_common::prepare_backend(render_common::EngineBackend::room, plan, logs, profile);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }
    Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::invalid_argument, "room-binaural prepared 类型错误");
        }
        return RoomStream::create(*prepared, plan);
    }
};
} // namespace
CapabilityReport room_capabilities() {
    return render_common::engine_capabilities(render_common::EngineBackend::room);
}
std::unique_ptr<IRenderer> create_room_renderer() {
    return std::make_unique<RoomRenderer>();
}
} // namespace mradm
