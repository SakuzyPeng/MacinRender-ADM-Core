#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>

#include "adm/errors.h"

#include "dsp_ffi.h"

namespace mradm::dsp {
class EarFilters {
  public:
    static constexpr std::size_t k_taps = 512U;
    [[nodiscard]] static Result<EarFilters>
    create(std::size_t channels, std::span<const float> firs, std::size_t delay) {
        EarFilters result;
        std::array<char, 256> message{};
        void* raw = nullptr;
        const auto status = mradm_dsp_ear_filters_create(
            channels, firs.data(), firs.size(), delay, &raw, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "EAR filter preparation");
        }
        result.handle_.reset(raw);
        return result;
    }
    [[nodiscard]] const void* get() const noexcept { return handle_.get(); }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_ear_filters_destroy)> handle_{nullptr, mradm_dsp_ear_filters_destroy};
};

class EarPostProcessor {
  public:
    EarPostProcessor(const EarFilters& filters, std::size_t max_frames) {
        std::array<char, 256> message{};
        void* raw = nullptr;
        check(mradm_dsp_ear_post_create(filters.get(), max_frames, &raw, message.data(), message.size()),
              message.data());
        handle_.reset(raw);
    }
    void reset() {
        std::array<char, 256> message{};
        check(mradm_dsp_ear_post_reset(handle_.get(), message.data(), message.size()), message.data());
    }
    void process(std::span<float> direct, std::span<const float> diffuse, std::size_t frames) {
        std::array<char, 256> message{};
        check(mradm_dsp_ear_post_process(handle_.get(),
                                         direct.data(),
                                         direct.size(),
                                         diffuse.data(),
                                         diffuse.size(),
                                         frames,
                                         message.data(),
                                         message.size()),
              message.data());
    }

  private:
    static void check(int status, const char* message) {
        if (status != 0) {
            throw std::runtime_error(message);
        }
    }
    std::unique_ptr<void, decltype(&mradm_dsp_ear_post_destroy)> handle_{nullptr, mradm_dsp_ear_post_destroy};
};
} // namespace mradm::dsp
