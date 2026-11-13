#pragma once

// The test drivers of the Rust backends (formerly production, PCM consistency 5.6 A): the one
// preparation entry, the offline driver and the file stream of every Rust backend, which the
// backend IRenderer shells in this directory and the C++ tests use. Production renders through
// mradm-engine directly.

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
#include "engine_records.h"
#include "engine_test_ffi.h"
#include "routing_test_ffi.h"

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

// Takes over an opened engine stream (the monitor's stream opening, 5.3 B). `input_path` locates
// its errors; `output_layout` is the plan's layout unless the stream reports binaural output.
[[nodiscard]] std::unique_ptr<IRenderStream> adopt_engine_stream(MradmEngineStream* raw,
                                                                 const MradmEngineStreamInfo& info,
                                                                 const std::string& input_path,
                                                                 std::string_view output_layout,
                                                                 RoomControlAdopter room_control);

// Gives up an engine stream's Rust handle (with its info) to a Rust monitor; null (and the stream
// untouched) for any other IRenderStream.
[[nodiscard]] MradmEngineStream* release_engine_stream(std::unique_ptr<IRenderStream>& stream,
                                                       MradmEngineStreamInfo& info);

} // namespace mradm::render_common
