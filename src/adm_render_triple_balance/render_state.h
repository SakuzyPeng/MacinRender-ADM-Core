#pragma once
#include <memory>
#include <span>
#include <vector>

#include "adm/render.h"

#include "../adm_dsp/triple_balance.h"
#include "speaker_pcm.h"
namespace mradm::triple_balance {
struct SizeTrack {
    uint16_t input_channel{};
    std::string object_id;
    bool has_diffuse{};
    float minimum_diffuse_size{1};
    float maximum_source_size{}; // Scalar summary for live override validation; events belong to Rust.
};
struct Prepared final : IPreparedRender {
    uint16_t output_channels{};
    uint32_t sample_rate{};
    SpeakerSpreadMode spread_mode{};
    render_common::PreparedPcmMix gain_matrix;
    dsp::TbPlan numeric;
    TripleBalanceMode mode{TripleBalanceMode::standard};
    dsp::TbHandle<mradm_dsp_tb_d_plan_destroy> d_numeric{nullptr, mradm_dsp_tb_d_plan_destroy};
    std::vector<SizeTrack> size_tracks;
};
class SizeMixer {
  public:
    static Result<SizeMixer> create(const Prepared& prepared, const RenderPlan& plan, bool live_points = false);
    Result<void> prepare_points(uint64_t start, std::size_t frames);
    void accumulate_point(std::size_t track,
                          const render_common::AccumulateContext& context,
                          std::size_t frames,
                          bool user_gain);
    Result<void> process(std::span<const float> source,
                         std::span<float> mixed,
                         uint64_t start,
                         bool final,
                         std::span<const float> live = {});
    void set_scales(std::span<const float> scales, bool immediate);
    void set_point_in_matrix(std::span<const float> scales);
    void reset();
    dsp::TbSnapshot snapshot() const;
    void restore(const dsp::TbSnapshot& snapshot);
    std::size_t snapshot_bytes() const;

  private:
    dsp::TbHandle<mradm_dsp_tb_destroy> handle_{nullptr, mradm_dsp_tb_destroy};
    std::size_t inputs_{};
};
Result<std::unique_ptr<IRenderStream>> open_stream(const Prepared& prepared,
                                                   const RenderPlan& plan,
                                                   std::size_t checkpoint_budget = std::size_t{32} * 1024 * 1024);
} // namespace mradm::triple_balance
