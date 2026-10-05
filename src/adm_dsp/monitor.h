#pragma once
#include <array>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>

#include "monitor_ffi.h"

namespace mradm::dsp {
// Prepared internal callers satisfy the contract (ADR 0005). No callback error allocation/logging.
inline void monitor_checked(int status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}
template <auto Destroy> using MonitorHandle = std::unique_ptr<void, decltype(Destroy)>;

class MonitorCrossfade {
  public:
    MonitorCrossfade(std::size_t channels, uint64_t frames) {
        void* raw = nullptr;
        std::array<char, 256> error{};
        if (mradm_dsp_monitor_crossfade_create(channels, frames, &raw, error.data(), error.size()) != 0) {
            throw std::runtime_error(error.data());
        }
        handle_.reset(raw);
    }
    void reset() noexcept { monitor_checked(mradm_dsp_monitor_crossfade_reset(handle_.get(), nullptr, 0)); }
    bool process(std::span<float> old_pcm, std::span<const float> incoming, std::size_t frames) noexcept {
        uint32_t complete = 0;
        monitor_checked(mradm_dsp_monitor_crossfade_process(handle_.get(),
                                                            old_pcm.data(),
                                                            old_pcm.size(),
                                                            incoming.data(),
                                                            incoming.size(),
                                                            frames,
                                                            &complete,
                                                            nullptr,
                                                            0));
        return complete != 0;
    }

  private:
    MonitorHandle<mradm_dsp_monitor_crossfade_destroy> handle_{nullptr, mradm_dsp_monitor_crossfade_destroy};
};

class MonitorOutput {
  public:
    MonitorOutput(std::size_t channels, uint32_t rate, bool realtime) {
        void* raw = nullptr;
        std::array<char, 256> error{};
        if (mradm_dsp_monitor_output_create(channels, rate, realtime ? 1U : 0U, &raw, error.data(), error.size()) !=
            0) {
            throw std::runtime_error(error.data());
        }
        handle_.reset(raw);
    }
    void reset() noexcept { monitor_checked(mradm_dsp_monitor_output_reset(handle_.get(), nullptr, 0)); }
    void process(std::span<float> pcm,
                 std::size_t frames,
                 std::size_t produced,
                 bool active,
                 uint64_t generation,
                 std::span<float> peak,
                 std::span<float> rms) noexcept {
        monitor_checked(mradm_dsp_monitor_output_process(handle_.get(),
                                                         pcm.data(),
                                                         pcm.size(),
                                                         frames,
                                                         produced,
                                                         active ? 1U : 0U,
                                                         generation,
                                                         peak.data(),
                                                         peak.size(),
                                                         rms.data(),
                                                         rms.size(),
                                                         nullptr,
                                                         0));
    }

  private:
    MonitorHandle<mradm_dsp_monitor_output_destroy> handle_{nullptr, mradm_dsp_monitor_output_destroy};
};
} // namespace mradm::dsp
