#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <AudioToolbox/AudioToolbox.h>
#include <fmt/format.h>

#include "adm/render_apple.h"

namespace mradm {
namespace {

constexpr double k_probe_sample_rate = 48000.0;
constexpr UInt32 k_probe_max_frames_per_slice = 512;

[[nodiscard]] std::string os_status_message(OSStatus status) {
    return fmt::format("OSStatus {}", static_cast<int>(status));
}

[[nodiscard]] Error apple_probe_error(std::string message, OSStatus status) {
    return {ErrorCode::unsupported, std::move(message), os_status_message(status)};
}

class AudioUnitGuard {
  public:
    explicit AudioUnitGuard(AudioUnit unit) : unit_(unit) {}
    AudioUnitGuard(const AudioUnitGuard&) = delete;
    AudioUnitGuard& operator=(const AudioUnitGuard&) = delete;
    AudioUnitGuard(AudioUnitGuard&& other) noexcept : unit_(other.unit_) { other.unit_ = nullptr; }
    AudioUnitGuard& operator=(AudioUnitGuard&& other) noexcept {
        if (this != &other) {
            dispose();
            unit_ = other.unit_;
            other.unit_ = nullptr;
        }
        return *this;
    }

    ~AudioUnitGuard() { dispose(); }

    [[nodiscard]] AudioUnit get() const noexcept { return unit_; }

  private:
    void dispose() noexcept {
        if (unit_ != nullptr) {
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }
    AudioUnit unit_{nullptr};
};

[[nodiscard]] Result<AudioUnitGuard> create_spatial_mixer_unit() {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Mixer;
    desc.componentSubType = kAudioUnitSubType_SpatialMixer;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        return tl::unexpected{
            Error{ErrorCode::unsupported, "AUSpatialMixer component is not available", "subtype=3dem"}};
    }

    AudioUnit unit = nullptr;
    const OSStatus status = AudioComponentInstanceNew(comp, &unit);
    if (status != noErr || unit == nullptr) {
        return tl::unexpected{apple_probe_error("failed to create AUSpatialMixer instance", status)};
    }

    return AudioUnitGuard{unit};
}

[[nodiscard]] AudioStreamBasicDescription pcm_float_format(UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = k_probe_sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved |
                       kAudioFormatFlagsNativeEndian;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

[[nodiscard]] Result<void> set_uint32_property(AudioUnit unit,
                                               AudioUnitPropertyID property,
                                               AudioUnitScope scope,
                                               AudioUnitElement element,
                                               UInt32 value,
                                               std::string_view label) {
    const OSStatus status = AudioUnitSetProperty(unit, property, scope, element, &value, sizeof(value));
    if (status != noErr) {
        return tl::unexpected{apple_probe_error(fmt::format("failed to set AUSpatialMixer {}", label), status)};
    }
    return {};
}

[[nodiscard]] Result<void> set_stream_format(
    AudioUnit unit, AudioUnitScope scope, AudioUnitElement element, UInt32 channels, std::string_view label) {
    auto fmt = pcm_float_format(channels);
    const OSStatus status =
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, &fmt, sizeof(fmt));
    if (status != noErr) {
        return tl::unexpected{apple_probe_error(fmt::format("failed to set AUSpatialMixer {}", label), status)};
    }
    return {};
}

[[nodiscard]] Result<void> configure_probe_unit(AudioUnit unit, uint16_t output_channels) {
    if (auto res = set_uint32_property(
            unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, 1, "input element count");
        !res) {
        return res;
    }
    if (auto res = set_uint32_property(unit,
                                       kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global,
                                       0,
                                       k_probe_max_frames_per_slice,
                                       "maximum frames per slice");
        !res) {
        return res;
    }
    if (auto res = set_stream_format(unit, kAudioUnitScope_Input, 0, 1, "mono input stream format"); !res) {
        return res;
    }
    if (auto res = set_stream_format(
            unit, kAudioUnitScope_Output, 0, static_cast<UInt32>(output_channels), "output stream format");
        !res) {
        return res;
    }
    if (auto res = set_uint32_property(unit,
                                       kAudioUnitProperty_SpatializationAlgorithm,
                                       kAudioUnitScope_Input,
                                       0,
                                       kSpatializationAlgorithm_HRTFHQ,
                                       "spatialization algorithm");
        !res) {
        return res;
    }
    if (auto res = set_uint32_property(unit,
                                       kAudioUnitProperty_SpatialMixerSourceMode,
                                       kAudioUnitScope_Input,
                                       0,
                                       kSpatialMixerSourceMode_PointSource,
                                       "source mode");
        !res) {
        return res;
    }

    const OSStatus status = AudioUnitInitialize(unit);
    if (status != noErr) {
        return tl::unexpected{apple_probe_error("failed to initialize AUSpatialMixer", status)};
    }
    return {};
}

[[nodiscard]] uint16_t probe_output_channels(const RenderPlan& plan) {
    const auto caps = apple_capabilities();
    const auto it = std::ranges::find_if(caps.supported_layouts, [&](const CapabilityReport::Layout& layout) {
        return layout.id == plan.output_layout;
    });
    if (it != caps.supported_layouts.end()) {
        return it->channel_count;
    }
    return 2;
}

struct ApplePrepared final : IPreparedRender {
    uint16_t output_channels{2};
};

class AppleRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return apple_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        const uint16_t output_channels = probe_output_channels(plan);
        auto unit = create_spatial_mixer_unit();
        if (!unit) {
            return tl::unexpected{unit.error()};
        }
        if (auto configured = configure_probe_unit(unit->get(), output_channels); !configured) {
            return tl::unexpected{configured.error()};
        }

        logs.log(
            LogLevel::info, "apple", fmt::format("AUSpatialMixer probe initialized ({}ch output)", output_channels));
        auto prepared = std::make_shared<ApplePrepared>();
        prepared->output_channels = output_channels;
        return std::static_pointer_cast<IPreparedRender>(prepared);
    }

    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& prepared, const RenderPlan&, ProgressSink&, LogSink&) override {
        if (dynamic_cast<const ApplePrepared*>(&prepared) == nullptr) {
            return tl::unexpected{
                Error{ErrorCode::internal_error, "prepared state does not belong to apple renderer", {}}};
        }
        return tl::unexpected{
            Error{ErrorCode::unsupported, "apple spatial mixer rendering is scaffolded but not implemented yet", {}}};
    }
};

} // namespace

CapabilityReport apple_capabilities() {
    CapabilityReport r;
    r.backend_name = "apple";
    r.backend_version = "spatial-mixer-scaffold";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_screen_ref = false;
    r.supports_diffuse = false;
    r.supports_render_window = false;
    r.supported_layouts = {
        {"binaural", "Apple AUSpatialMixer binaural", 2, false, 0, true, true},
        {"5.1.4", "Apple AUSpatialMixer 5.1.4", 10, true, 1, true, false},
        {"7.1.4", "Apple AUSpatialMixer 7.1.4", 12, true, 1, true, false},
    };
    return r;
}

std::unique_ptr<IRenderer> create_apple_renderer() {
    return std::make_unique<AppleRenderer>();
}

} // namespace mradm
