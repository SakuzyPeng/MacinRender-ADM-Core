#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/render.h"

namespace mradm::realtime {

// A stream decorator that presents an inner IRenderStream's output in a different (monitor)
// channel count by applying a fixed downmix / upmix matrix per frame. This is how the
// monitor plays a multichannel / HOA render on a fixed-channel physical device (e.g. 7.1.4
// or HOA → stereo headphones): the engine only ever sees streams in the monitor channel
// count, so ring / device / crossfade need no channel-count awareness. The matrix is
// layout-agnostic here (built from layout geometry one level up); this class is pure math.
// process / seek / set_overrides delegate to the inner stream (so live edits still reach the
// object-addressed backend); out_channels() reports the monitor count.
class DownmixStream final : public IRenderStream {
  public:
    // `matrix` is row-major [monitor_channels][inner->out_channels()]:
    // out[frame*monitor + d] = sum_s matrix[d*src + s] * inner_out[frame*src + s].
    DownmixStream(std::unique_ptr<IRenderStream> inner, std::vector<float> matrix, uint32_t monitor_channels)
        : inner_(std::move(inner)), matrix_(std::move(matrix)), src_channels_(inner_->out_channels()),
          monitor_channels_(monitor_channels) {}

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        const std::size_t src_floats = frames * src_channels_;
        if (scratch_.size() < src_floats) {
            scratch_.assign(src_floats, 0.0F);
        }
        auto produced = inner_->process(std::span<float>(scratch_.data(), src_floats), frames);
        if (!produced) {
            return tl::unexpected{produced.error()};
        }
        const std::size_t got = *produced;
        for (std::size_t f = 0; f < got; ++f) {
            const float* in = scratch_.data() + (f * src_channels_);
            float* dst = out.data() + (f * monitor_channels_);
            for (uint32_t d = 0; d < monitor_channels_; ++d) {
                const float* row = matrix_.data() + (static_cast<std::size_t>(d) * src_channels_);
                float acc = 0.0F;
                for (uint32_t s = 0; s < src_channels_; ++s) {
                    acc += row[s] * in[s];
                }
                dst[d] = acc;
            }
        }
        return got;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override { return inner_->seek(frame); }
    void set_overrides(const LiveOverrides& overrides) override { inner_->set_overrides(overrides); }
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        inner_->set_listener_orientation(orientation);
    }

    [[nodiscard]] uint32_t out_channels() const override { return monitor_channels_; }
    [[nodiscard]] uint32_t sample_rate() const override { return inner_->sample_rate(); }
    [[nodiscard]] std::string_view output_layout() const override { return inner_->output_layout(); }

  private:
    std::unique_ptr<IRenderStream> inner_;
    std::vector<float> matrix_;  // monitor_channels_ × src_channels_, row-major
    std::vector<float> scratch_; // inner output staging (src_channels_ × frames)
    uint32_t src_channels_;
    uint32_t monitor_channels_;
};

} // namespace mradm::realtime
