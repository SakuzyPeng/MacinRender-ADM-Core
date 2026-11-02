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
// across offline windows and file streams (PreviewSession scrubbing). It keeps the plan's input
// format and the Triple Balance mode for the stream checks.
struct EnginePrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderPrepared, decltype(&mradm_render_prepared_destroy)> handle{
        nullptr, &mradm_render_prepared_destroy};
    uint16_t outputs{0};
    uint16_t inputs{0};
    uint32_t sample_rate{0};
    uint64_t frames{0};
    TripleBalanceMode mode{TripleBalanceMode::standard};
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

// File streaming over a loudspeaker preparation (mradm-render `stream`): the offline block size and
// renderer into a FIFO, so a gap-free run from frame 0 is bit-identical to the offline render. Live
// gain pre-scales each input channel by its object's (or speaker label's) override; topology
// scales (diffuse / extent / divergence) are ignored.
[[nodiscard]] Result<std::unique_ptr<IRenderStream>> open_file_stream(const EnginePrepared& prepared,
                                                                      const RenderPlan& plan);

} // namespace mradm::render_common
