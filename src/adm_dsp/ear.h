#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"

#include "ear_ffi.h"

namespace mradm::dsp {
struct EarLayout {
    std::string name;
    std::vector<MradmEarChannel> channels;
};
class EarFailure final : public std::runtime_error {
  public:
    EarFailure(int status, const char* message) : std::runtime_error(message), code_(static_cast<ErrorCode>(status)) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};
inline void ear_check(int status, const std::array<uint8_t, 512>& message) {
    if (status != 0) {
        throw EarFailure(status, reinterpret_cast<const char*>(message.data()));
    }
}
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): accepts a C ABI array field.
template <size_t N> void ear_string(uint8_t (&out)[N], std::string_view value) {
    if (value.size() >= N) {
        throw EarFailure(1, "EAR string too long");
    }
    std::fill(std::begin(out), std::end(out), 0);
    std::copy(value.begin(), value.end(), out);
}
inline void ear_string(MradmEarLabel& out, std::string_view value) {
    out.value = reinterpret_cast<const uint8_t*>(value.data());
    out.length = value.size();
}
inline std::string_view ear_channel_name(const MradmEarChannel& channel) {
    return {reinterpret_cast<const char*>(channel.name)};
}
inline EarLayout ear_layout(std::string_view name) {
    EarLayout result{std::string{name}, std::vector<MradmEarChannel>(256)};
    size_t count{};
    std::array<uint8_t, 512> message{};
    ear_check(mradm_ear_layout(reinterpret_cast<const uint8_t*>(name.data()),
                               name.size(),
                               result.channels.data(),
                               result.channels.size(),
                               &count,
                               message.data(),
                               message.size()),
              message);
    result.channels.resize(count);
    return result;
}
class EarCalculator {
  public:
    explicit EarCalculator(const EarLayout& layout) : channels_(layout.channels.size()) {
        std::array<uint8_t, 512> message{};
        void* handle = nullptr;
        ear_check(mradm_ear_create(reinterpret_cast<const uint8_t*>(layout.name.data()),
                                   layout.name.size(),
                                   layout.channels.data(),
                                   layout.channels.size(),
                                   &handle,
                                   message.data(),
                                   message.size()),
                  message);
        handle_.reset(handle);
    }
    void objects(const MradmEarObject& metadata, std::span<double> direct, std::span<double> diffuse) const {
        if (direct.size() != channels_ || diffuse.size() != channels_) {
            throw EarFailure(1, "EAR gain dimensions");
        }
        std::array<uint8_t, 512> message{};
        ear_check(
            mradm_ear_objects(
                handle_.get(), &metadata, direct.data(), diffuse.data(), channels_, message.data(), message.size()),
            message);
    }
    void direct_speakers(const MradmEarDirect& metadata,
                         std::span<const MradmEarLabel> labels,
                         std::span<double> gains) const {
        if (gains.size() != channels_) {
            throw EarFailure(1, "EAR gain dimensions");
        }
        std::array<uint8_t, 512> message{};
        ear_check(mradm_ear_direct(handle_.get(),
                                   &metadata,
                                   labels.data(),
                                   labels.size(),
                                   gains.data(),
                                   channels_,
                                   message.data(),
                                   message.size()),
                  message);
    }
    [[nodiscard]] std::vector<double>
    hoa(std::span<const int32_t> orders, std::span<const int32_t> degrees, std::string_view normalization) const {
        if (orders.size() != degrees.size() || orders.size() > SIZE_MAX / channels_) {
            throw EarFailure(1, "EAR HOA dimensions");
        }
        uint32_t norm = 3;
        if (normalization == "N3D") {
            norm = 0;
        } else if (normalization == "SN3D") {
            norm = 1;
        } else if (normalization == "FuMa") {
            norm = 2;
        }
        std::vector<double> result(orders.size() * channels_);
        std::array<uint8_t, 512> message{};
        ear_check(mradm_ear_hoa(handle_.get(),
                                orders.data(),
                                degrees.data(),
                                orders.size(),
                                norm,
                                result.data(),
                                result.size(),
                                message.data(),
                                message.size()),
                  message);
        return result;
    }
    [[nodiscard]] std::vector<float> filters() const {
        std::vector<float> result(channels_ * 512U);
        std::array<uint8_t, 512> message{};
        ear_check(mradm_ear_filters(handle_.get(), result.data(), result.size(), message.data(), message.size()),
                  message);
        return result;
    }

  private:
    size_t channels_;
    std::unique_ptr<void, decltype(&mradm_ear_destroy)> handle_{nullptr, mradm_ear_destroy};
};
} // namespace mradm::dsp
