#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>

#include "dsp_ffi.h"

namespace mradm::dsp {

// Private prepared owner. All targets and buffers belong to the calling render thread.
class GainBank final {
  public:
    GainBank(std::size_t channels, std::uint32_t rate, std::uint32_t ramp_ms) {
        std::array<char, 256> error{};
        if (mradm_dsp_gain_create(channels, rate, ramp_ms, &handle_, error.data(), error.size()) != 0) {
            throw std::runtime_error(error.data());
        }
    }
    ~GainBank() { mradm_dsp_gain_destroy(handle_); }
    GainBank(const GainBank&) = delete;
    GainBank& operator=(const GainBank&) = delete;
    GainBank(GainBank&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    GainBank& operator=(GainBank&& other) noexcept {
        if (this != &other) {
            mradm_dsp_gain_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    void reset() noexcept { check(mradm_dsp_gain_reset(handle_, nullptr, 0U)); }
    void set_targets(std::span<const float> targets) noexcept {
        check(mradm_dsp_gain_set_targets(handle_, targets.data(), targets.size(), nullptr, 0U));
    }
    void fill(std::span<float> output) noexcept {
        check(mradm_dsp_gain_process(handle_, output.data(), output.size(), 0U, nullptr, 0U));
    }
    void apply(std::span<float> pcm) noexcept {
        check(mradm_dsp_gain_process(handle_, pcm.data(), pcm.size(), 1U, nullptr, 0U));
    }

  private:
    // Prepared callers satisfy the contract; violations in noexcept DSP are programming errors (ADR 0005).
    static void check(int status) noexcept {
        if (status != 0) {
            std::terminate();
        }
    }
    void* handle_{nullptr};
};
} // namespace mradm::dsp
