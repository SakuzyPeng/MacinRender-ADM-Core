#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/live_override.h"
#include "adm/render.h"

#include "../adm_dsp/pcm_mix.h"
#include "routing_ffi.h"

namespace mradm::render_common {

struct BlockGains {
    std::vector<float> gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
    std::optional<uint64_t> interp_length_samples;
};

// One input channel with its full sorted block sequence.
struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::string object_id;          // owning SceneObject::id, for live gain overrides
    std::string speaker_label_key;  // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
    float output_gain{1.0F};
};

struct ChannelBinding {
    uint16_t input_channel{0};
    std::string object_id;
    std::string speaker_label_key;
};

// A Rust offline driver plan of `tracks` input channels: a PcmMixer plan (kernel 0, prepared in
// C++ or by a Rust backend) or an HOA3 encoder plan (kernel 1).
struct SpeakerMix {
    const void* plan{nullptr};
    std::size_t tracks{0};
    uint32_t kernel{0};
};

struct PreparedPcmMix {
    std::vector<ChannelBinding> channels;
    dsp::PcmMixPlan plan;
    // NOLINTNEXTLINE(google-explicit-constructor): every prepared mix is a driver plan.
    operator SpeakerMix() const { return {plan.get(), channels.size()}; }
    [[nodiscard]] auto begin() const { return channels.begin(); }
    [[nodiscard]] auto end() const { return channels.end(); }
    [[nodiscard]] auto size() const { return channels.size(); }
    [[nodiscard]] bool empty() const { return channels.empty(); }
    [[nodiscard]] const ChannelBinding& operator[](std::size_t index) const { return channels[index]; }
};

[[nodiscard]] Result<PreparedPcmMix>
prepare_speaker_mix(std::vector<ChannelGainInfo> channels, std::size_t inputs, std::size_t outputs);

struct AccumulateContext {
    const float* input{nullptr};
    std::vector<float>* output{nullptr};
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
    uint64_t object_smoothing_frames{0};
    // Optional sample-domain output gain per input channel, after spatial/user gain.
    std::span<const float> live_gains{};
};

// Optional per-render stateful DSP. Adds to interleaved output after the gain mix.
// A non-empty processor requires warming from frame zero for cropped output.
using SpeakerBlockProcessor =
    std::function<Result<void>(std::span<const float> input, std::span<float> output, bool end_of_input)>;

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

// A VBAP or HOA plan prepared in Rust mradm-render; immutable and reused across render_window()
// and open_stream() calls (PreviewSession scrubbing).
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
                                                                   uint32_t kernel,
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
                                                       LogSink& logs,
                                                       const SpeakerBlockProcessor& process = {});

} // namespace mradm::render_common
