#pragma once
#include <array>
#include <exception>
#include <memory>
#include <span>

#include "adm/errors.h"

#include "live_vbap_ffi.h"
namespace mradm::dsp {
class LiveVbapMixer {
  public:
    static Result<LiveVbapMixer> create(uint32_t elements, uint32_t channels) {
        LiveVbapMixer result;
        std::array<char, 256> error{};
        void* raw = nullptr;
        const int status = mradm_dsp_live_vbap_create(elements, channels, &raw, error.data(), error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "Live VBAP DSP");
        }
        result.handle_.reset(raw);
        return result;
    }
    void reset() noexcept {
        if (mradm_dsp_live_vbap_reset(handle_.get(), nullptr, 0) != 0) {
            std::terminate();
        }
    }
    Result<void> process(uint32_t frames,
                         std::span<const MradmLiveVbapPlane> planes,
                         std::span<const MradmLiveVbapCommand> initial,
                         std::span<const MradmLiveVbapCommand> events,
                         std::span<const float> coefficients,
                         std::span<float> output) {
        std::array<char, 256> error{};
        const int status = mradm_dsp_live_vbap_process(handle_.get(),
                                                       frames,
                                                       planes.data(),
                                                       planes.size(),
                                                       initial.data(),
                                                       initial.size(),
                                                       events.data(),
                                                       events.size(),
                                                       coefficients.data(),
                                                       coefficients.size(),
                                                       output.data(),
                                                       output.size(),
                                                       error.data(),
                                                       error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "Live VBAP DSP");
        }
        return {};
    }
    const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_live_vbap_destroy)> handle_{nullptr, mradm_dsp_live_vbap_destroy};
};
} // namespace mradm::dsp
