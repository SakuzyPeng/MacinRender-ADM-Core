#pragma once
#include "adm/errors.h"

#include "live_binaural_ffi.h"
#include "scene_math.h"
namespace mradm::dsp {
class LiveBinauralSession {
  public:
    static Result<LiveBinauralSession> create(const void* filters,
                                              std::span<const MradmLiveBinauralDescription> descriptions,
                                              uint32_t rate,
                                              uint32_t spread) {
        LiveBinauralSession result;
        void* raw = nullptr;
        std::array<char, 256> message{};
        const auto status = mradm_dsp_live_binaural_create(filters,
                                                           descriptions.data(),
                                                           descriptions.size(),
                                                           rate,
                                                           spread,
                                                           scene_cpp_contract != 0 ? 1U : 0U,
                                                           &raw,
                                                           message.data(),
                                                           message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "Live binaural DSP");
        }
        result.handle_.reset(raw);
        return result;
    }
    void reset() noexcept { scene_check(mradm_dsp_live_binaural_reset(handle_.get())); }
    Result<MradmLiveBinauralReport> process(uint32_t frames,
                                            std::span<const MradmLiveBinauralPlane> planes,
                                            std::span<const MradmLiveBinauralCommand> initial,
                                            std::span<const MradmLiveBinauralCommand> events,
                                            std::array<float, 3> pose,
                                            uint32_t warned,
                                            std::span<float> output) {
        std::array<char, 256> message{};
        MradmLiveBinauralReport report{};
        const auto status = mradm_dsp_live_binaural_process(handle_.get(),
                                                            frames,
                                                            planes.data(),
                                                            planes.size(),
                                                            initial.data(),
                                                            initial.size(),
                                                            events.data(),
                                                            events.size(),
                                                            pose.data(),
                                                            pose.size(),
                                                            warned,
                                                            output.data(),
                                                            output.size(),
                                                            &report,
                                                            message.data(),
                                                            message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "Live binaural DSP");
        }
        return report;
    }
    const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_live_binaural_destroy)> handle_{nullptr, mradm_dsp_live_binaural_destroy};
};
} // namespace mradm::dsp
