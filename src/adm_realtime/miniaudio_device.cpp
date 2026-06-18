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
#include <utility>

#include "audio_output_device.h"

namespace mradm::realtime {

namespace {

class MiniaudioDevice final : public IAudioOutputDevice {
  public:
    explicit MiniaudioDevice(bool use_null_backend) : use_null_backend_(use_null_backend) {}

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
    PullFn pull_;
    ma_context context_{};
    ma_device device_{};
    bool have_context_{false};
    bool have_device_{false};
    bool started_{false};
    uint32_t actual_sample_rate_{0};
};

} // namespace

std::unique_ptr<IAudioOutputDevice> make_miniaudio_device(bool use_null_backend) {
    return std::make_unique<MiniaudioDevice>(use_null_backend);
}

} // namespace mradm::realtime
