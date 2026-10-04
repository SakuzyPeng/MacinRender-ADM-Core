#pragma once

#include <memory>
#include <span>
#include <vector>

#include "adm/render.h"

#include "size_processor.h"
#include "speaker_pcm.h"

namespace mradm::triple_balance {

struct SizeTrack {
    uint16_t input_channel{};
    std::vector<SizeEvent> events;
    float output_gain{1.0F};
    std::string object_id;
    bool has_diffuse{};
    float minimum_diffuse_size{1.0F};
    SceneBlockPosition initial_position;
};

struct PointMotionState {
    SizePosition position;
    SizePosition target;
    std::array<float, 24> gains{};
    std::size_t next_event{1};
    uint64_t control_start{};
};

struct SizeTrackState {
    SizeProcessorState size;
    PointMotionState point;
};

struct Prepared final : IPreparedRender {
    uint16_t output_channels{};
    uint32_t sample_rate{};
    SpeakerSpreadMode spread_mode{};
    std::vector<render_common::ChannelGainInfo> gain_matrix;
    std::vector<SizeTrack> size_tracks;
};

// Common offline/streaming size mix. Live envelopes are applied after each object's DSP.
class SizeMixer {
  public:
    static Result<SizeMixer> create(const Prepared& prepared, const RenderPlan& plan, bool live_points = false);
    Result<void> prepare_points(uint64_t start_frame, std::size_t frames);
    void accumulate_point(std::size_t track,
                          const render_common::AccumulateContext& context,
                          std::size_t frames,
                          bool user_gain);
    Result<void> process(std::span<const float> source,
                         std::span<float> mixed,
                         uint64_t start_frame,
                         bool final,
                         std::span<const float> live_gains = {});
    void set_scales(std::span<const float> scales, bool immediate);
    void set_point_in_matrix(std::span<const float> scales);
    void reset();
    [[nodiscard]] std::vector<SizeTrackState> snapshot() const;
    void restore(std::span<const SizeTrackState> states);

  private:
    const Prepared* prepared_{};
    uint16_t input_channels_{};
    uint64_t default_interp_{};
    std::vector<SizeObjectProcessor> processors_;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<float> point_output_;
    std::vector<float> weights_;
    std::vector<float> targets_;
    std::vector<bool> point_in_matrix_;
    std::string layout_;
    std::vector<PointMotionState> initial_points_;
    std::vector<PointMotionState> points_;
    std::vector<render_common::ChannelGainInfo> point_channels_;
};

Result<std::unique_ptr<IRenderStream>> open_stream(const Prepared& prepared,
                                                   const RenderPlan& plan,
                                                   std::size_t checkpoint_budget = std::size_t{32} * 1024 * 1024);

} // namespace mradm::triple_balance
