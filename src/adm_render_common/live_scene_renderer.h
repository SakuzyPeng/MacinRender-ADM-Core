#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"

namespace mradm::live_scene {

// This module-private interface is implemented and consumed in separate renderer translation units;
// cppcheck cannot see those field uses while checking the header alone.
// cppcheck-suppress-begin unusedStructMember
enum class ElementRole : std::uint8_t { object = 0, direct_speaker = 1, lfe = 2 };

enum StateField : std::uint64_t {
    state_active = 1ULL << 0U,
    state_linear_gain = 1ULL << 1U,
    state_position = 1ULL << 2U,
    state_extent = 1ULL << 3U,
    state_diffuse = 1ULL << 4U,
    state_divergence = 1ULL << 5U,
    state_channel_lock = 1ULL << 6U,
    state_screen_reference = 1ULL << 7U,
    state_head_locked = 1ULL << 8U,
};

inline constexpr std::uint64_t k_known_state_fields = state_active | state_linear_gain | state_position | state_extent |
                                                      state_diffuse | state_divergence | state_channel_lock |
                                                      state_screen_reference | state_head_locked;

struct ObjectState {
    std::uint64_t valid_fields{0};
    bool active{true};
    float linear_gain{1.0F};
    float x{0.0F};
    float y{1.0F};
    float z{0.0F};
    float width{0.0F};
    float height{0.0F};
    float depth{0.0F};
    float diffuse{0.0F};
    float divergence{0.0F};
    bool channel_lock{false};
    bool screen_reference{false};
    bool head_locked{false};
};

struct ElementDescriptor {
    std::uint64_t element_id{0};
    ElementRole role{ElementRole::object};
    std::string speaker_label;
    bool has_position{false};
    float x{0.0F};
    float y{1.0F};
    float z{0.0F};
    std::uint64_t flags{0};
};

struct PcmPlane {
    std::uint64_t element_id{0};
    bool has_signal{false};
    std::vector<float> samples;
};

struct StateEntry {
    std::uint64_t element_id{0};
    ObjectState state;
};

struct MetadataUpdate {
    std::uint64_t element_id{0};
    std::uint32_t offset_samples{0};
    std::uint32_t ramp_duration_samples{0};
    std::uint64_t changed_fields{0};
    ObjectState state;
    std::uint64_t stream_order{0};
};

enum FrameFlag : std::uint32_t {
    frame_state_complete = 1U << 0U,
    frame_warmup = 1U << 1U,
    frame_discontinuity = 1U << 2U,
    frame_concealed = 1U << 3U,
};

struct Frame {
    std::uint64_t epoch_id{0};
    std::uint64_t generation_id{0};
    std::int64_t media_sample_start{0};
    std::uint32_t duration_samples{0};
    std::uint32_t flags{0};
    std::vector<PcmPlane> pcm;
    std::vector<StateEntry> initial_states;
    std::vector<MetadataUpdate> updates;
};

struct RendererConfig {
    RendererSelection renderer{RendererSelection::saf};
    std::string output_layout;
    std::filesystem::path sofa_path;
    SpeakerGeometry speaker_geometry{SpeakerGeometry::standard};
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    LfeRoutingMode lfe_routing_mode{LfeRoutingMode::direct};
    std::uint32_t sample_rate{48000U};
};

enum class DiagnosticCode : std::uint32_t {
    none = 0,
    semantic_degraded = 1,
    direct_speaker_fallback = 2,
    missing_lfe_output = 3,
    incomplete_state = 4,
    timeline_discontinuity = 5,
    backend_failure = 6,
};

struct Diagnostic {
    LogLevel level{LogLevel::warning};
    DiagnosticCode code{DiagnosticCode::none};
    std::uint64_t epoch_id{0};
    std::uint64_t generation_id{0};
    std::uint64_t element_id{0};
    std::uint64_t field_mask{0};
    std::string message;
};

using DiagnosticSink = std::function<void(Diagnostic)>;

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

    [[nodiscard]] virtual std::uint32_t output_channels() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sample_rate() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t tail_input_frames() const noexcept = 0;

  protected:
    ILiveSceneRenderer() = default;
};
// cppcheck-suppress-end unusedStructMember

} // namespace mradm::live_scene
