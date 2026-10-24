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

#include "routing_ffi.h"

namespace mradm::render_common {

// The driver's log, progress and error wording: the loudspeaker backends', or the HOA encoder's
// and EAR's own.
enum class DriverStyle : uint8_t { speaker, hoa, ear };

// A Rust backend's offline driver plan (its preparation's kernel) of `tracks` input channels.
struct SpeakerMix {
    const void* plan{nullptr};
    std::size_t tracks{0};
    DriverStyle style{DriverStyle::speaker};
};

// A Rust driver or stream error with the locator its kind names (routing_ffi.h).
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

// A VBAP, HOA, EAR or Triple Balance plan prepared in Rust mradm-render; immutable and reused across
// render_window() and open_stream() calls (PreviewSession scrubbing).
struct RustPrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderPrepared, decltype(&mradm_render_prepared_destroy)> handle{
        nullptr, &mradm_render_prepared_destroy};
    SpeakerMix mix;
    uint16_t outputs{0};
    uint16_t inputs{0};
    uint32_t sample_rate{0};
};

// Takes ownership of a Rust preparation, logs its lines under `module` and returns its error or
// the prepared plan.
[[nodiscard]] Result<std::shared_ptr<RustPrepared>> adopt_prepared(int32_t code,
                                                                   MradmRenderPrepared* raw,
                                                                   const MradmRenderPreparedView& view,
                                                                   DriverStyle style,
                                                                   LogSink& logs,
                                                                   std::string_view module);

// File streaming over a Rust preparation (mradm-render `stream`): the offline block size and
// renderer into a FIFO, so a gap-free run from frame 0 is bit-identical to render_window. Live
// gain pre-scales each input channel by its object's (or speaker label's) override; topology
// scales (diffuse / extent / divergence) are ignored.
[[nodiscard]] Result<std::unique_ptr<IRenderStream>> open_file_stream(const RustPrepared& prepared,
                                                                      const RenderPlan& plan);

[[nodiscard]] Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                                       const SpeakerMix& gain_matrix,
                                                       uint16_t num_out_ch,
                                                       std::string_view backend,
                                                       ProgressSink& progress,
                                                       LogSink& logs);

} // namespace mradm::render_common
