#pragma once
// The C++ Scene stream renderer interface. Since PCM consistency 5.4 B the Rust stream engine calls
// the Rust renderers directly; the tests, the frozen references and the manual benchmarks still
// drive one renderer frame by frame through it.
#include <cstdint>
#include <memory>
#include <span>

#include "live_scene_renderer.h"

namespace mradm::live_scene {

// Scene streaming renderer: consumes PCM and metadata in caller-submitted frames.
class ILiveSceneRenderer {
  public:
    virtual ~ILiveSceneRenderer() = default;
    ILiveSceneRenderer(const ILiveSceneRenderer&) = delete;
    ILiveSceneRenderer& operator=(const ILiveSceneRenderer&) = delete;
    ILiveSceneRenderer(ILiveSceneRenderer&&) = delete;
    ILiveSceneRenderer& operator=(ILiveSceneRenderer&&) = delete;

    [[nodiscard]] virtual Result<void> configure_generation(std::uint64_t generation_id,
                                                            std::span<const ElementDescriptor> elements) = 0;
    virtual void reset() = 0;
    [[nodiscard]] virtual Result<void> render(const Frame& frame, std::span<float> interleaved_output) = 0;
    virtual void set_listener_orientation(const ListenerOrientation& orientation) { (void) orientation; }
    [[nodiscard]] virtual std::shared_ptr<IRoomControl> room_control() const { return {}; }
    // Called once before EOS flushing. Existing renderers retain their established quota.
    [[nodiscard]] virtual Result<std::uint64_t> end_input() { return std::uint64_t{0}; }

    [[nodiscard]] virtual std::uint32_t output_channels() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sample_rate() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t tail_input_frames() const noexcept = 0;

  protected:
    ILiveSceneRenderer() = default;
};

} // namespace mradm::live_scene
