#pragma once
#include <array>
#include <exception>
#include <memory>
#include <span>

#include "adm/errors.h"

#include "live_triple_balance_ffi.h"
namespace mradm::dsp {
class LiveTripleBalanceMixer {
  public:
    static Result<LiveTripleBalanceMixer>
    create(uint32_t layout, uint32_t rate, std::span<const MradmTbLiveElement> elements) {
        LiveTripleBalanceMixer result;
        std::array<char, 256> error{};
        void* raw = nullptr;
        const int status =
            mradm_dsp_live_tb_create(layout, rate, elements.data(), elements.size(), &raw, error.data(), error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "Live Triple Balance DSP");
        }
        result.handle_.reset(raw);
        return result;
    }
    void reset() noexcept {
        if (mradm_dsp_live_tb_reset(handle_.get(), nullptr, 0) != 0) {
            std::terminate();
        }
    }
    Result<void> process(uint32_t frames,
                         std::span<const MradmTbLivePlane> planes,
                         std::span<const MradmTbLiveCommand> initial,
                         std::span<const MradmTbLiveCommand> events,
                         std::span<float> output) {
        std::array<char, 256> error{};
        const int status = mradm_dsp_live_tb_process(handle_.get(),
                                                     frames,
                                                     planes.data(),
                                                     planes.size(),
                                                     initial.data(),
                                                     initial.size(),
                                                     events.data(),
                                                     events.size(),
                                                     output.data(),
                                                     output.size(),
                                                     error.data(),
                                                     error.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), error.data(), "Live Triple Balance DSP");
        }
        return {};
    }
    const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_live_tb_destroy)> handle_{nullptr, mradm_dsp_live_tb_destroy};
};
} // namespace mradm::dsp
