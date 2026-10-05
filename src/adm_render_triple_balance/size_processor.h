#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"

#include "../adm_dsp/triple_balance.h"
#include "size_panner.h"
namespace mradm::triple_balance {
// Low-level adapters retained for tests/tools; production processes whole render blocks in Rust.
class SizeDecorrelator final {
  public:
    using FilteredFrame = std::array<float, 4>;
    static constexpr std::size_t subblock_frames = 32;
    SizeDecorrelator();
    void reset() noexcept;
    void process(std::span<const float, 32> input, std::span<FilteredFrame, 32> output) noexcept;

  private:
    dsp::TbHandle<mradm_dsp_tb_filter_destroy> handle_{nullptr, mradm_dsp_tb_filter_destroy};
};
struct SizeEvent {
    uint64_t start_sample{};
    SizePosition position;
    float size{};
};
using SizeProcessorState = dsp::TbHandle<mradm_dsp_tb_object_snapshot_destroy>;
class SizeObjectProcessor final {
  public:
    static Result<SizeObjectProcessor>
    create(std::span<const SizeEvent> events, const std::string& layout, uint32_t rate);
    void reset() noexcept;
    Result<void> push(std::span<const float> input, std::vector<float>& output);
    Result<void> finish(std::vector<float>& output);
    void set_size_scale(float scale) noexcept;
    SizeProcessorState snapshot() const;
    MradmTbObjectStatus state_info() const;
    void restore(const SizeProcessorState& state) noexcept;
    std::size_t channel_count() const noexcept { return channels_; }

  private:
    Result<void> append(std::span<const float> input, std::vector<float>& output, bool final_block);
    dsp::TbHandle<mradm_dsp_tb_object_destroy> handle_{nullptr, mradm_dsp_tb_object_destroy};
    std::size_t channels_{};
};
} // namespace mradm::triple_balance
