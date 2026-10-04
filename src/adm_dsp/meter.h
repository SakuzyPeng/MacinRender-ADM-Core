#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "adm/errors.h"

#include "dsp_ffi.h"

namespace mradm::dsp {

enum class MeterMode : uint32_t { integrated, true_peak, integrated_true_peak, monitor };
enum class MeterChannel : uint32_t {
    unused,
    left,
    right,
    center,
    left_surround,
    right_surround,
    side_left,
    side_right,
    rear_left,
    rear_right,
    top_front_left,
    top_front_right,
    top_rear_left,
    top_rear_right
};

// Movable owner of a Rust meter. A single worker must serialize all operations;
// full-history integration can allocate during long playback. PCM is borrowed
// only for add_frames(), and queued callers must keep both buffers and owner alive.
class Meter final {
  public:
    [[nodiscard]] static Result<Meter>
    create(uint32_t channels, uint32_t sample_rate, MeterMode mode, std::span<const MeterChannel> channel_map = {}) {
        if (channel_map.size() > 64U) {
            return make_error(ErrorCode::invalid_argument, "meter channel map is too large");
        }
        std::array<uint32_t, 64> positions{};
        for (std::size_t i = 0; i < channel_map.size(); ++i) {
            positions.at(i) = static_cast<uint32_t>(channel_map[i]);
        }
        std::array<char, 256> message{};
        void* handle = nullptr;
        const auto status = mradm_dsp_meter_create(channels,
                                                   sample_rate,
                                                   static_cast<uint32_t>(mode),
                                                   positions.data(),
                                                   channel_map.size(),
                                                   &handle,
                                                   message.data(),
                                                   message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "meter");
        }
        return Meter(handle, channels);
    }

    ~Meter() { mradm_dsp_meter_destroy(handle_); }
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    Meter(Meter&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)), channels_(std::exchange(other.channels_, 0U)) {}
    Meter& operator=(Meter&& other) noexcept {
        if (this != &other) {
            mradm_dsp_meter_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
            channels_ = std::exchange(other.channels_, 0U);
        }
        return *this;
    }

    [[nodiscard]] Result<void> add_frames(const float* samples, std::size_t frames) {
        if (channels_ == 0U || frames > std::numeric_limits<std::size_t>::max() / channels_) {
            return make_error(ErrorCode::invalid_argument, "invalid meter frame count or moved-from meter");
        }
        std::array<char, 256> message{};
        const auto status = mradm_dsp_meter_add(handle_, samples, frames * channels_, message.data(), message.size());
        return status_result(status, message.data());
    }

    [[nodiscard]] Result<void> reset() {
        std::array<char, 256> message{};
        const auto status = mradm_dsp_meter_reset(handle_, message.data(), message.size());
        return status_result(status, message.data());
    }

    [[nodiscard]] Result<double> integrated() const { return query(0U); }
    [[nodiscard]] Result<double> momentary() const { return query(1U); }
    [[nodiscard]] Result<double> shortterm() const { return query(2U); }
    [[nodiscard]] Result<double> true_peak(uint32_t channel) const { return query(3U, channel); }
    [[nodiscard]] Result<double> max_true_peak() const { return query(4U); }

  private:
    Meter(void* handle, uint32_t channels) : handle_(handle), channels_(channels) {}
    [[nodiscard]] static Result<void> status_result(int32_t status, const char* message) {
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message, "meter");
        }
        return {};
    }
    [[nodiscard]] Result<double> query(uint32_t kind, uint32_t channel = 0U) const {
        std::array<char, 256> message{};
        double value = 0.0;
        const auto status = mradm_dsp_meter_query(handle_, kind, channel, &value, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "meter");
        }
        return value;
    }
    void* handle_;
    uint32_t channels_;
};

} // namespace mradm::dsp
