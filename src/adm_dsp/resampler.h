#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "adm/errors.h"

#include "dsp_ffi.h"

namespace mradm::dsp {

// Private movable Rust owner. Operations belong to the preparation/worker
// thread, with exclusive access to the handle and borrowed audio buffers.
class Resampler final {
  public:
    [[nodiscard]] static Result<Resampler> create(std::size_t channels, uint32_t input_rate, uint32_t output_rate) {
        std::array<char, 256> message{};
        void* handle = nullptr;
        const auto status =
            mradm_dsp_resampler_create(channels, input_rate, output_rate, &handle, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "resampler");
        }
        return Resampler(handle);
    }
    ~Resampler() { mradm_dsp_resampler_destroy(handle_); }
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Resampler& operator=(Resampler&& other) noexcept {
        if (this != &other) {
            mradm_dsp_resampler_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] Result<void> reset() {
        std::array<char, 256> message{};
        const auto status = mradm_dsp_resampler_reset(handle_, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "resampler");
        }
        return {};
    }
    [[nodiscard]] Result<MradmDspResampleProgress> process(std::span<const float> input, std::span<float> output) {
        return run(input, output, false);
    }
    [[nodiscard]] Result<std::size_t> finish(std::span<float> output) {
        auto progress = run({}, output, true);
        if (!progress) {
            return tl::unexpected{progress.error()};
        }
        return progress->output_frames;
    }

  private:
    explicit Resampler(void* handle) : handle_(handle) {}
    [[nodiscard]] Result<MradmDspResampleProgress>
    run(std::span<const float> input, std::span<float> output, bool end) {
        std::array<char, 256> message{};
        MradmDspResampleProgress progress{};
        const auto status = mradm_dsp_resampler_process(handle_,
                                                        input.data(),
                                                        input.size(),
                                                        output.data(),
                                                        output.size(),
                                                        end ? 1U : 0U,
                                                        &progress,
                                                        message.data(),
                                                        message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "resampler");
        }
        return progress;
    }
    void* handle_;
};
} // namespace mradm::dsp
