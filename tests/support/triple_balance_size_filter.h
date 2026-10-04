#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "adm/errors.h"

#include "dsp.h"

namespace mradm::triple_balance {

// Historical measured FIR comparison; never linked into the production renderer.
// Four layout-independent filter modes from the early size experiment. The caller supplies spatial gains separately. A
// SizeFilterBank belongs to one object and retains its own convolution state across process() calls.
class SizeFilterBank final {
  public:
    static constexpr std::size_t mode_count = 4;
    static constexpr std::size_t block_frames = 8192;
    static constexpr std::size_t filter_frames = 8192;

    [[nodiscard]] static Result<std::unique_ptr<SizeFilterBank>>
    create(const std::array<std::vector<float>, mode_count>& filters);

    SizeFilterBank(const SizeFilterBank&) = delete;
    SizeFilterBank& operator=(const SizeFilterBank&) = delete;
    SizeFilterBank(SizeFilterBank&&) = delete;
    SizeFilterBank& operator=(SizeFilterBank&&) = delete;
    ~SizeFilterBank();

    // Output is interleaved [frame][mode]. Input may be shorter than
    // block_frames, including a final partial block; state remains continuous.
    [[nodiscard]] Result<void> process(std::span<const float> input, std::span<float> filtered);
    void reset() noexcept;

  private:
    SizeFilterBank();

    dsp::FftHandle fft_{nullptr};
    std::array<std::vector<std::complex<float>>, mode_count> filter_fd_;
    std::array<std::vector<float>, mode_count> overlap_;
    std::vector<float> source_time_;
    std::vector<std::complex<float>> source_fd_;
    std::vector<std::complex<float>> output_fd_;
    std::vector<float> output_time_;
    std::vector<float> next_overlap_;
};

} // namespace mradm::triple_balance
