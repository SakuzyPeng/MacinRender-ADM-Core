#pragma once

// Manual probe only: these miniaudio types never enter a library/API header.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <miniaudio.h>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio_output_device.h"

namespace room_probe {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

inline Json distribution(std::vector<double> values) {
    if (values.empty()) {
        return {{"count", 0}};
    }
    std::ranges::sort(values);
    const auto percentile = [&](double p) {
        return values[static_cast<std::size_t>(p * static_cast<double>(values.size() - 1))];
    };
    return {{"count", values.size()},
            {"min", values.front()},
            {"p50", percentile(.5)},
            {"p99", percentile(.99)},
            {"p999", percentile(.999)},
            {"max", values.back()}};
}

struct Histogram {
    // Fixed 20 us bins; the last bin contains longer values, with exact max kept.
    std::array<std::uint64_t, 5001> bins{};
    std::uint64_t count{0};
    double maximum_ms{0};
    void add(double ms) noexcept {
        ++bins[static_cast<std::size_t>(std::clamp(ms * 50., 0., 5000.))];
        ++count;
        maximum_ms = std::max(maximum_ms, ms);
    }
    [[nodiscard]] Json report() const {
        const auto percentile = [&](double p) {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < bins.size(); ++i) {
                sum += bins[i];
                if (sum > static_cast<std::uint64_t>(p * static_cast<double>(count))) {
                    return static_cast<double>(i) / 50.;
                }
            }
            return maximum_ms;
        };
        return {{"count", count},
                {"p50_ms", percentile(.5)},
                {"p99_ms", percentile(.99)},
                {"p999_ms", percentile(.999)},
                {"max_ms", maximum_ms},
                {"bin_width_ms", .02}};
    }
};

struct Window {
    std::int64_t first_ns{0};
    std::uint32_t frames{0};
    std::uint32_t maximum_callback_frames{0};
};
class Recording {
  public:
    Recording(Clock::time_point origin, unsigned seconds, bool enabled)
        : origin_(origin), first_second_(seconds > 20 ? 10U : 2U),
          windows_(enabled ? ((seconds - first_second_) / 60U) + 1U : 0U),
          samples_(windows_.size() * 48000U * 2U, 0.F) {
        // Commit recording pages before measuring RSS; logging must not look
        // like a growing renderer merely because later windows are first touched.
        for (std::size_t i = 0; i < samples_.size(); i += 1024U) {
            volatile float& value = samples_[i];
            value = 0.F;
        }
    }
    void capture(std::span<const float> samples, std::size_t frames, Clock::time_point now) noexcept {
        const double time = std::chrono::duration<double>(now - origin_).count();
        if (windows_.empty() || time < first_second_) {
            return;
        }
        const auto index = static_cast<std::size_t>((time - first_second_) / 60.);
        if (index >= windows_.size() || time - first_second_ - static_cast<double>(index) * 60. > 2.) {
            return;
        }
        auto& window = windows_[index];
        if (window.frames == 0) {
            window.first_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin_).count();
        }
        const auto count = std::min<std::size_t>({frames, 48000U - window.frames, samples.size() / 2U});
        const auto start = ((index * 48000U) + window.frames) * 2U;
        std::copy_n(samples.begin(), count * 2U, samples_.begin() + static_cast<std::ptrdiff_t>(start));
        window.frames += static_cast<std::uint32_t>(count);
        window.maximum_callback_frames = std::max(window.maximum_callback_frames, static_cast<std::uint32_t>(frames));
    }
    [[nodiscard]] Json save(const std::filesystem::path& root, const std::string& name) const {
        Json rows = Json::array();
        for (std::size_t i = 0; i < windows_.size(); ++i) {
            const auto& w = windows_[i];
            rows.push_back({{"first_callback_ns", w.first_ns},
                            {"frames", w.frames},
                            {"maximum_callback_frames", w.maximum_callback_frames},
                            {"offset_frames", i * 48000U}});
        }
        if (!windows_.empty()) {
            std::ofstream file(root / (name + ".f32"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(samples_.data()),
                       static_cast<std::streamsize>(samples_.size() * sizeof(float)));
            if (!file) {
                throw std::runtime_error("Cannot save probe recording");
            }
        }
        return {{"file", name + ".f32"},
                {"sample_rate", 48000},
                {"channels", 2},
                {"windows", rows},
                {"timestamp", "steady clock at callback entry; first sample presentation/capture time is unknown"}};
    }

  private:
    Clock::time_point origin_;
    unsigned first_second_;
    std::vector<Window> windows_;
    std::vector<float> samples_;
};
struct OutputTrace {
    OutputTrace(Clock::time_point origin, unsigned seconds, bool record) : recording(origin, seconds, record) {}
    Histogram intervals;
    Histogram pull_duration;
    Recording recording;
    Clock::time_point previous{};
    std::uint64_t valid_frames{0};
    std::uint64_t callbacks{0};
};
class MeasuredDevice final : public mradm::realtime::IAudioOutputDevice {
  public:
    MeasuredDevice(std::unique_ptr<IAudioOutputDevice> device, std::shared_ptr<OutputTrace> trace)
        : device_(std::move(device)), trace_(std::move(trace)) {}
    ~MeasuredDevice() override { device_->stop(); }
    mradm::Result<void> start(std::uint32_t channels, std::uint32_t rate, PullFn pull) override {
        return device_->start(channels, rate, [this, pull = std::move(pull)](std::span<float> out, std::size_t frames) {
            const auto now = Clock::now();
            if (trace_->callbacks != 0) {
                trace_->intervals.add(std::chrono::duration<double, std::milli>(now - trace_->previous).count());
            }
            trace_->previous = now;
            ++trace_->callbacks;
            const auto result = pull(out, frames);
            trace_->pull_duration.add(std::chrono::duration<double, std::milli>(Clock::now() - now).count());
            trace_->valid_frames += result;
            // The production device pads short pulls after this wrapper returns.
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(result * 2U), out.end(), 0.F);
            trace_->recording.capture(out, frames, now);
            return result;
        });
    }
    void stop() override { device_->stop(); }
    [[nodiscard]] std::uint32_t actual_sample_rate() const override { return device_->actual_sample_rate(); }
    [[nodiscard]] mradm::realtime::AudioDeviceProgress progress() const override { return device_->progress(); }

  private:
    std::unique_ptr<IAudioOutputDevice> device_;
    std::shared_ptr<OutputTrace> trace_;
};

class CaptureTap {
  public:
    CaptureTap(Clock::time_point origin,
               unsigned seconds,
               const std::string& name,
               const std::string& loopback_output_token)
        : recording_(origin, seconds, true) {
        if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
            throw std::runtime_error("Capture context initialization failed");
        }
        context_ready_ = true;
        try {
            ma_device_info* devices = nullptr;
            ma_uint32 count = 0;
            const bool loopback = !loopback_output_token.empty();
            auto result = loopback ? ma_context_get_devices(&context_, &devices, &count, nullptr, nullptr)
                                   : ma_context_get_devices(&context_, nullptr, nullptr, &devices, &count);
            if (result != MA_SUCCESS) {
                throw std::runtime_error("Capture device enumeration failed");
            }
            std::size_t matches = 0;
            for (ma_uint32 i = 0; i < count; ++i) {
                if ((!loopback && name == devices[i].name) ||
                    (loopback && token(devices[i].id) == loopback_output_token)) {
                    id_ = devices[i].id;
                    name_ = devices[i].name;
                    ++matches;
                }
            }
            if (matches != 1) {
                throw std::runtime_error("Capture selection must match exactly one device");
            }
            auto config = ma_device_config_init(loopback ? ma_device_type_loopback : ma_device_type_capture);
            config.capture.pDeviceID = &id_;
            config.capture.format = ma_format_f32;
            config.capture.channels = 2;
            config.sampleRate = 48000;
            config.pUserData = this;
            config.dataCallback = [](ma_device* device, void*, const void* input, ma_uint32 frames) {
                if (input != nullptr) {
                    auto& self = *static_cast<CaptureTap*>(device->pUserData);
                    self.recording_.capture({static_cast<const float*>(input), static_cast<std::size_t>(frames) * 2U},
                                            frames,
                                            Clock::now());
                }
            };
            if (ma_device_init(&context_, &config, &device_) != MA_SUCCESS) {
                throw std::runtime_error("Selected capture/loopback device could not be opened");
            }
            device_ready_ = true;
            if (ma_device_start(&device_) != MA_SUCCESS) {
                throw std::runtime_error("Capture start failed");
            }
        } catch (...) {
            stop();
            throw;
        }
    }
    ~CaptureTap() { stop(); }
    CaptureTap(const CaptureTap&) = delete;
    CaptureTap& operator=(const CaptureTap&) = delete;
    void stop() noexcept {
        if (device_ready_) {
            ma_device_uninit(&device_);
            device_ready_ = false;
        }
        if (context_ready_) {
            ma_context_uninit(&context_);
            context_ready_ = false;
        }
    }
    [[nodiscard]] Json save(const std::filesystem::path& root) const {
        auto result = recording_.save(root, "capture");
        result["device_name"] = name_;
        return result;
    }
    [[nodiscard]] static Json inputs() {
        ma_context context{};
        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
            throw std::runtime_error("Capture enumeration failed");
        }
        ma_device_info* devices = nullptr;
        ma_uint32 count = 0;
        Json result = Json::array();
        if (ma_context_get_devices(&context, nullptr, nullptr, &devices, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                result.push_back({{"name", devices[i].name}, {"default", devices[i].isDefault != MA_FALSE}});
            }
        }
        ma_context_uninit(&context);
        return result;
    }

  private:
    static std::string token(const ma_device_id& id) {
        constexpr std::string_view digits = "0123456789abcdef";
        const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
        std::string result;
        for (std::size_t i = 0; i < sizeof(id); ++i) {
            result.push_back(digits[bytes[i] >> 4U]);
            result.push_back(digits[bytes[i] & 15U]);
        }
        return result;
    }
    ma_context context_{};
    ma_device device_{};
    ma_device_id id_{};
    bool context_ready_{false};
    bool device_ready_{false};
    std::string name_;
    Recording recording_;
};
} // namespace room_probe
