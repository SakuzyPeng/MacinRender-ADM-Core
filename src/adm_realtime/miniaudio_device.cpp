// This translation unit owns the miniaudio implementation (single-header library). All
// miniaudio types stay inside this file; the rest of the engine only sees the
// IAudioOutputDevice interface (ADR 0003). miniaudio.h is a SYSTEM include, so its
// implementation's own warnings do not surface here.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <miniaudio.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio_output_device.h"

namespace mradm::realtime {

namespace {

// Serialize an opaque ma_device_id (a backend-specific fixed-size union) to a lowercase hex
// token, and back. The token round-trips a specific device across enumerate→open within a
// session on the same machine/backend; it is not portable and not meant to be (a stale token
// simply fails to match and we fall back to the default device).
std::string device_id_to_token(const ma_device_id& id) {
    static constexpr char k_hex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    std::string token;
    token.reserve(sizeof(ma_device_id) * 2);
    for (std::size_t i = 0; i < sizeof(ma_device_id); ++i) {
        token.push_back(k_hex[bytes[i] >> 4]);
        token.push_back(k_hex[bytes[i] & 0x0F]);
    }
    return token;
}

// Parse a hex token back into a ma_device_id. Returns false on any malformed input.
bool token_to_device_id(const std::string& token, ma_device_id& out) {
    if (token.size() != sizeof(ma_device_id) * 2) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return (c - 'a') + 10;
        }
        return -1;
    };
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (std::size_t i = 0; i < sizeof(ma_device_id); ++i) {
        const int hi = nibble(token[i * 2]);
        const int lo = nibble(token[(i * 2) + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

class MiniaudioDevice final : public IAudioOutputDevice {
  public:
    MiniaudioDevice(bool use_null_backend, std::string device_id)
        : use_null_backend_(use_null_backend), device_id_(std::move(device_id)) {}

    ~MiniaudioDevice() override { stop(); }
    MiniaudioDevice(const MiniaudioDevice&) = delete;
    MiniaudioDevice& operator=(const MiniaudioDevice&) = delete;
    MiniaudioDevice(MiniaudioDevice&&) = delete;
    MiniaudioDevice& operator=(MiniaudioDevice&&) = delete;

    Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) override {
        if (started_) {
            return make_error(ErrorCode::invalid_argument, "audio device already started");
        }
        pull_ = std::move(pull);

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = channels;
        config.sampleRate = sample_rate;
        config.dataCallback = &data_callback;
        config.pUserData = this;

        // A specific device requested: resolve the token to a device id and point miniaudio at
        // it (resolved_id_ must outlive ma_device_init). A malformed/stale token leaves
        // pDeviceID null → the system default device (graceful fallback).
        if (!device_id_.empty() && token_to_device_id(device_id_, resolved_id_)) {
            config.playback.pDeviceID = &resolved_id_;
        }

        // The null backend gives a hardware-free, timer-driven device for headless / CI.
        if (use_null_backend_) {
            ma_context_config ctx_config = ma_context_config_init();
            const std::array<ma_backend, 1> backends{ma_backend_null};
            if (ma_context_init(backends.data(), static_cast<ma_uint32>(backends.size()), &ctx_config, &context_) !=
                MA_SUCCESS) {
                return make_error(ErrorCode::internal_error, "miniaudio null context init failed");
            }
            have_context_ = true;
        }

        if (ma_device_init(have_context_ ? &context_ : nullptr, &config, &device_) != MA_SUCCESS) {
            teardown();
            return make_error(ErrorCode::internal_error, "miniaudio device init failed");
        }
        have_device_ = true;
        actual_sample_rate_ = device_.sampleRate;

        if (ma_device_start(&device_) != MA_SUCCESS) {
            teardown();
            return make_error(ErrorCode::internal_error, "miniaudio device start failed");
        }
        started_ = true;
        return {};
    }

    void stop() override { teardown(); }

    [[nodiscard]] uint32_t actual_sample_rate() const override { return actual_sample_rate_; }

  private:
    // Audio thread: drain the engine's pull into miniaudio's output buffer. Wait-free
    // (pull pops a ring); pads with silence if the producer underran.
    static void data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
        auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
        auto* out = static_cast<float*>(output);
        const uint32_t channels = device->playback.channels;
        const std::size_t total = static_cast<std::size_t>(frame_count) * channels;
        std::size_t produced = 0;
        if (self != nullptr && self->pull_) {
            produced = self->pull_(std::span<float>(out, total), frame_count);
        }
        if (produced < frame_count) {
            std::fill(out + (produced * channels), out + total, 0.0F);
        }
    }

    void teardown() noexcept {
        if (have_device_) {
            ma_device_uninit(&device_);
            have_device_ = false;
        }
        if (have_context_) {
            ma_context_uninit(&context_);
            have_context_ = false;
        }
        started_ = false;
    }

    bool use_null_backend_;
    std::string device_id_;      // opaque token; empty = default device
    ma_device_id resolved_id_{}; // parsed token, referenced by config during ma_device_init
    PullFn pull_;
    ma_context context_{};
    ma_device device_{};
    bool have_context_{false};
    bool have_device_{false};
    bool started_{false};
    uint32_t actual_sample_rate_{0};
};

} // namespace

std::vector<AudioDeviceInfo> enumerate_output_devices() {
    std::vector<AudioDeviceInfo> result;
    ma_context context{};
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        return result; // no enumeration → caller uses the default device
    }

    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    if (ma_context_get_devices(&context, &playback, &playback_count, nullptr, nullptr) == MA_SUCCESS &&
        playback != nullptr) {
        result.reserve(playback_count);
        for (ma_uint32 i = 0; i < playback_count; ++i) {
            const ma_device_info& info = playback[i];
            result.push_back(
                AudioDeviceInfo{device_id_to_token(info.id), std::string{info.name}, info.isDefault != MA_FALSE});
        }
    }
    ma_context_uninit(&context);
    return result;
}

std::unique_ptr<IAudioOutputDevice> make_miniaudio_device(bool use_null_backend, const std::string& device_id) {
    return std::make_unique<MiniaudioDevice>(use_null_backend, device_id);
}

} // namespace mradm::realtime
