#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "adm/errors.h"

#include "dsp_ffi.h"

namespace mradm::dsp {
inline void check_mix(int status, const char* message) {
    if (status != 0) {
        throw std::runtime_error(message);
    }
}

class PcmMixPlan {
  public:
    [[nodiscard]] static Result<PcmMixPlan> create(std::size_t inputs,
                                                   std::size_t outputs,
                                                   std::span<const MradmDspMixRow> rows,
                                                   std::span<const MradmDspMixBlock> blocks,
                                                   std::span<const float> gains,
                                                   std::span<const double> ear_gains = {},
                                                   bool ear = false) {
        PcmMixPlan result;
        std::array<char, 256> error{};
        void* raw = nullptr;
        const auto status = mradm_dsp_mix_plan_create(inputs,
                                                      outputs,
                                                      rows.data(),
                                                      rows.size(),
                                                      blocks.data(),
                                                      blocks.size(),
                                                      gains.data(),
                                                      gains.size(),
                                                      ear_gains.data(),
                                                      ear_gains.size(),
                                                      ear ? 1U : 0U,
                                                      &raw,
                                                      error.data(),
                                                      error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "PCM mix preparation");
        }
        result.handle_.reset(raw);
        return result;
    }
    [[nodiscard]] const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_mix_plan_destroy)> handle_{nullptr, mradm_dsp_mix_plan_destroy};
};

class PcmMixer {
  public:
    PcmMixer(const PcmMixPlan& plan, std::size_t frames, std::uint64_t interpolation, bool smoothing) {
        std::array<char, 256> error{};
        void* raw = nullptr;
        check_mix(mradm_dsp_mix_create(
                      plan.get(), frames, interpolation, smoothing ? 1U : 0U, &raw, error.data(), error.size()),
                  error.data());
        handle_.reset(raw);
    }
    static PcmMixer dynamic(std::size_t inputs,
                            std::size_t outputs,
                            std::span<const std::size_t> channels,
                            std::size_t blocks,
                            std::size_t frames,
                            std::uint64_t interpolation) {
        PcmMixer result;
        std::array<char, 256> error{};
        void* raw = nullptr;
        check_mix(mradm_dsp_mix_dynamic_create(inputs,
                                               outputs,
                                               channels.data(),
                                               channels.size(),
                                               blocks,
                                               frames,
                                               interpolation,
                                               &raw,
                                               error.data(),
                                               error.size()),
                  error.data());
        result.handle_.reset(raw);
        return result;
    }
    void reset() {
        std::array<char, 256> error{};
        check_mix(mradm_dsp_mix_reset(handle_.get(), error.data(), error.size()), error.data());
    }
    void
    update(std::size_t row, std::span<const MradmDspMixBlock> blocks, std::span<const float> gains, float output_gain) {
        std::array<char, 256> error{};
        check_mix(mradm_dsp_mix_update(handle_.get(),
                                       row,
                                       blocks.data(),
                                       blocks.size(),
                                       gains.data(),
                                       gains.size(),
                                       output_gain,
                                       error.data(),
                                       error.size()),
                  error.data());
    }
    void speaker(std::span<const float> input,
                 std::span<float> output,
                 std::span<const float> live,
                 std::uint64_t start,
                 std::size_t frames,
                 std::size_t row = std::numeric_limits<std::size_t>::max(),
                 std::optional<float> gain = {}) {
        std::array<char, 256> error{};
        check_mix(mradm_dsp_mix_speaker(handle_.get(),
                                        row,
                                        input.data(),
                                        input.size(),
                                        output.data(),
                                        output.size(),
                                        live.data(),
                                        live.size(),
                                        start,
                                        frames,
                                        gain ? 1U : 0U,
                                        gain.value_or(1.0F),
                                        error.data(),
                                        error.size()),
                  error.data());
    }
    void ear(std::span<const float> input,
             std::span<float> direct,
             std::span<float> diffuse,
             std::uint64_t start,
             std::size_t frames) {
        std::array<char, 256> error{};
        check_mix(mradm_dsp_mix_ear(handle_.get(),
                                    input.data(),
                                    input.size(),
                                    direct.data(),
                                    direct.size(),
                                    diffuse.data(),
                                    diffuse.size(),
                                    start,
                                    frames,
                                    error.data(),
                                    error.size()),
                  error.data());
    }

  private:
    PcmMixer() = default;
    std::unique_ptr<void, decltype(&mradm_dsp_mix_destroy)> handle_{nullptr, mradm_dsp_mix_destroy};
};

class PcmMatrix {
  public:
    PcmMatrix(std::size_t inputs, std::size_t outputs, std::span<const float> gains) {
        std::array<char, 256> error{};
        void* raw = nullptr;
        check_mix(
            mradm_dsp_matrix_create(inputs, outputs, gains.data(), gains.size(), &raw, error.data(), error.size()),
            error.data());
        handle_.reset(raw);
    }
    Result<void> process(std::span<const float> input, std::span<float> output, std::size_t frames) const {
        std::array<char, 256> error{};
        const auto status = mradm_dsp_matrix_process(handle_.get(),
                                                     input.data(),
                                                     input.size(),
                                                     output.data(),
                                                     output.size(),
                                                     frames,
                                                     error.data(),
                                                     error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "PCM matrix");
        }
        return {};
    }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_matrix_destroy)> handle_{nullptr, mradm_dsp_matrix_destroy};
};
} // namespace mradm::dsp
