#include "render_state.h"
namespace mradm::triple_balance {
Result<SizeMixer> SizeMixer::create(const Prepared& prepared, const RenderPlan& plan, bool live) {
    SizeMixer result;
    void* raw = nullptr;
    auto status = dsp::tb_status(mradm_dsp_tb_create,
                                 prepared.numeric.get(),
                                 std::size_t{1024},
                                 uint64_t{plan.scene.info.sample_rate} * plan.default_interp_ms / 1000,
                                 live ? 1U : 0U,
                                 &raw);
    if (!status) {
        return tl::unexpected{status.error()};
    }
    result.handle_.reset(raw);
    result.inputs_ = plan.scene.info.num_channels;
    return result;
}
void SizeMixer::set_scales(std::span<const float> scales, bool immediate) {
    dsp::tb_checked(mradm_dsp_tb_scales, handle_.get(), scales.data(), scales.size(), immediate ? 1U : 0U);
}
void SizeMixer::set_point_in_matrix(std::span<const float> scales) {
    dsp::tb_checked(mradm_dsp_tb_scales, handle_.get(), scales.data(), scales.size(), 2U);
}
void SizeMixer::reset() {
    dsp::tb_checked(mradm_dsp_tb_reset, handle_.get());
}
Result<void> SizeMixer::prepare_points(uint64_t start, std::size_t frames) {
    return dsp::tb_status(mradm_dsp_tb_prepare_points, handle_.get(), start, frames);
}
void SizeMixer::accumulate_point(std::size_t track,
                                 const render_common::AccumulateContext& c,
                                 std::size_t frames,
                                 bool user_gain) {
    dsp::tb_checked(mradm_dsp_tb_point,
                    handle_.get(),
                    track,
                    c.input,
                    frames * c.num_in_ch,
                    c.output->data(),
                    c.output->size(),
                    c.live_gains.data(),
                    c.live_gains.size(),
                    c.frames_done,
                    frames,
                    user_gain ? 1U : 0U);
}
Result<void> SizeMixer::process(
    std::span<const float> source, std::span<float> mixed, uint64_t start, bool final, std::span<const float> live) {
    return dsp::tb_status(mradm_dsp_tb_process,
                          handle_.get(),
                          source.data(),
                          source.size(),
                          mixed.data(),
                          mixed.size(),
                          live.data(),
                          live.size(),
                          start,
                          source.size() / inputs_,
                          final ? 1U : 0U);
}
dsp::TbSnapshot SizeMixer::snapshot() const {
    void* raw = nullptr;
    dsp::tb_checked(mradm_dsp_tb_snapshot_create, handle_.get(), &raw);
    return dsp::TbSnapshot{raw, mradm_dsp_tb_snapshot_destroy};
}
void SizeMixer::restore(const dsp::TbSnapshot& s) {
    dsp::tb_checked(mradm_dsp_tb_snapshot_restore, handle_.get(), s.get());
}
std::size_t SizeMixer::snapshot_bytes() const {
    std::size_t bytes = 0;
    dsp::tb_checked(mradm_dsp_tb_snapshot_bytes, handle_.get(), &bytes);
    return bytes;
}
} // namespace mradm::triple_balance
