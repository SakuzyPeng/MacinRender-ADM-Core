#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/live_override.h"
#include "adm/render.h"

#include "capability_records.h"
#include "engine_ffi.h"
#include "routing_ffi.h"

namespace mradm::render_common {

// A Rust stream error with the locator its kind names (routing_ffi.h).
class DriverError {
  public:
    uint32_t* location() noexcept { return &location_; }
    uint8_t* message() noexcept { return message_.data(); }
    std::size_t* message_len() noexcept {
        message_len_ = message_.size();
        return &message_len_;
    }
    [[nodiscard]] tl::unexpected<Error> error(int32_t code, const std::string& input_path) const;

  private:
    std::array<uint8_t, 512> message_{};
    std::size_t message_len_{0};
    uint32_t location_{0};
};

[[nodiscard]] inline MradmAdmText text(std::string_view value) noexcept {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// A DirectSpeakers matrix as Rust routing records, borrowing the matrix's labels.
struct MatrixRecords {
    explicit MatrixRecords(const DirectSpeakersMatrix& matrix);
    std::vector<std::vector<MradmRenderMatrixTarget>> targets;
    std::vector<MradmRenderMatrixRoute> routes;
    std::size_t label_bytes{0};
};

// A Rust backend preparation (mradm-engine `backend`, PCM consistency 5.2 C1): immutable and reused
// across offline windows and file streams (PreviewSession scrubbing). The Rust preparation keeps
// the input format the stream checks compare (5.3 A).
struct EnginePrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderPrepared, decltype(&mradm_render_prepared_destroy)> handle{
        nullptr, &mradm_render_prepared_destroy};
};

// A screen as the Rust room record.
[[nodiscard]] MradmRoomScreenGeometry screen_geometry(const ScreenGeometry& screen);

// The one preparation entry of the Rust backends (also for MonitorEngine and the stream tools):
// the plan's backend checks, logs and the backend's own preparation run in Rust. `room_profile` is
// the room package handle of plan.room (room-binaural only).
[[nodiscard]] Result<std::shared_ptr<EnginePrepared>>
prepare_backend(EngineBackend backend, const RenderPlan& plan, LogSink& logs, const void* room_profile = nullptr);

// The offline render of a Rust backend preparation (mradm-engine `offline`): reading, rendering,
// writing, metering, the progress events, logs and cancellation points run in Rust; this loop
// forwards them and passes the stop token before every step.
[[nodiscard]] Result<RenderMetrics>
render_offline(const IPreparedRender& prepared, const RenderPlan& plan, ProgressSink& progress, LogSink& logs);

// Adopts a room control handle (the room backend's IRoomControl shell).
using RoomControlAdopter = std::shared_ptr<IRoomControl> (*)(MradmRenderRoomControl*);

// The file stream of a Rust backend preparation (mradm-engine `stream`, PCM consistency 5.3 A):
// the backend's open checks, the FIFO, seek steps and their cancellation, live edits and their
// validation run in Rust. `room_control` adopts a room stream's control handle.
[[nodiscard]] Result<std::unique_ptr<IRenderStream>>
open_engine_stream(const EnginePrepared& prepared, const RenderPlan& plan, RoomControlAdopter room_control = nullptr);

} // namespace mradm::render_common
