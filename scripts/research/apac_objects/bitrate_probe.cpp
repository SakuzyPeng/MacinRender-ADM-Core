// Read-only bitrate observation around the existing native codec probe.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>

namespace {
bool observe_encoder_rates = false;
int requested_rate_mode = -1;
int requested_vbr_quality = -1;

void inspect_rates(AudioCodec codec, const char* phase) {
    for (const auto property : {kAudioCodecPropertyCurrentTargetBitRate,
                                kAudioCodecPropertyBitRateControlMode,
                                kAudioCodecPropertySoundQualityForVBR,
                                kAudioCodecPropertyBitRateForVBR,
                                kAudioCodecPropertyMaximumPacketByteSize}) {
        UInt32 value = 0, bytes = sizeof(value);
        const auto result = AudioCodecGetProperty(codec, property, &bytes, &value);
        std::printf("{\"stage\":\"bitrate_property\",\"phase\":\"%s\",\"property\":%u,\"status\":%d,"
                    "\"value\":%u}\n",
                    phase,
                    property,
                    static_cast<int>(result),
                    value);
    }
    const AudioCodecPropertyID range_properties[] = {kAudioCodecPropertyAvailableBitRateRange,
                                                     kAudioCodecPropertyApplicableBitRateRange,
                                                     kAudioCodecPropertyRecommendedBitRateRange};
    for (const auto property : range_properties) {
        UInt32 bytes = 0;
        auto result = AudioCodecGetPropertyInfo(codec, property, &bytes, nullptr);
        std::vector<AudioValueRange> ranges;
        if (result == noErr && bytes && bytes <= 65536 && bytes % sizeof(AudioValueRange) == 0) {
            ranges.resize(bytes / sizeof(AudioValueRange));
            result = AudioCodecGetProperty(codec, property, &bytes, ranges.data());
            if (result == noErr)
                ranges.resize(bytes / sizeof(AudioValueRange));
        }
        std::printf("{\"stage\":\"bitrate_range\",\"phase\":\"%s\",\"property\":%u,\"status\":%d,"
                    "\"ranges\":[",
                    phase,
                    property,
                    static_cast<int>(result));
        if (result == noErr) {
            for (std::size_t i = 0; i < ranges.size(); ++i)
                std::printf("%s[%.0f,%.0f]", i ? "," : "", ranges[i].mMinimum, ranges[i].mMaximum);
        }
        std::puts("]}");
    }
    std::fflush(stdout);
}

OSStatus observed_initialize(AudioCodec codec,
                             const AudioStreamBasicDescription* input,
                             const AudioStreamBasicDescription* output,
                             const void* cookie,
                             UInt32 cookie_bytes) {
    if (observe_encoder_rates) {
        inspect_rates(codec, "before_mode_settings");
        for (const auto& setting : {std::pair{kAudioCodecPropertyBitRateControlMode, requested_rate_mode},
                                    std::pair{kAudioCodecPropertySoundQualityForVBR, requested_vbr_quality}}) {
            if (setting.second >= 0) {
                const UInt32 value = static_cast<UInt32>(setting.second);
                const auto result = AudioCodecSetProperty(codec, setting.first, sizeof(value), &value);
                std::printf("{\"stage\":\"set_rate_mode_or_quality\",\"property\":%u,\"value\":%u,\"status\":%d}\n",
                            setting.first,
                            value,
                            static_cast<int>(result));
                if (result != noErr)
                    return result;
            }
        }
        inspect_rates(codec, "before_initialize");
    }
    const auto result = AudioCodecInitialize(codec, input, output, cookie, cookie_bytes);
    if (observe_encoder_rates && result == noErr)
        inspect_rates(codec, "after_initialize");
    return result;
}
} // namespace

// Reuse the established encode/drain/container logic and unchanged default
// decoder. The wrapper observes only encoder instances unless a rate mode or
// VBR quality is explicitly supplied in its command line.
#define AudioCodecInitialize observed_initialize
#define main original_codec_probe_main
#include "codec_probe.cpp"
#undef main
#undef AudioCodecInitialize

int main(int argc, char** argv) {
    if (argc == 11 && std::strcmp(argv[1], "encode-rate") == 0) {
        observe_encoder_rates = true;
        requested_rate_mode = std::atoi(argv[2]);
        requested_vbr_quality = std::atoi(argv[3]);
        char command[] = "encode";
        char* forwarded[] = {argv[0], command, argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10]};
        return original_codec_probe_main(9, forwarded);
    }
    if (argc > 1 && std::strcmp(argv[1], "encode") == 0)
        observe_encoder_rates = true;
    return original_codec_probe_main(argc, argv);
}
