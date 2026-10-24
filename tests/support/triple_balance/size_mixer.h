#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "adm/errors.h"

#include "speaker_mix.h"
#include "triple_balance.h"

// A test-only adapter over the Triple Balance size session (mradm_dsp_tb_*), for comparisons with
// the frozen legacy state; the file stream itself is Rust mradm-render (PCM consistency 4.6).
namespace mradm::triple_balance {
using render_common::AccumulateContext;
class SizeMixer {
  public:
    // `numeric` is a dsp::TbPlan; `interpolation` is the default ramp in frames.
    static Result<SizeMixer>
    create(const void* numeric, std::size_t inputs, uint64_t interpolation, bool live_points = false);
    Result<void> prepare_points(uint64_t start, std::size_t frames);
    void accumulate_point(std::size_t track, const AccumulateContext& context, std::size_t frames, bool user_gain);
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
} // namespace mradm::triple_balance
