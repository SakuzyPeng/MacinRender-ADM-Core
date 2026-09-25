// Render only decoder-derived PCM and positions, using AUSpatialMixer offline.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>

struct Position {
    unsigned long long sample;
    unsigned object;
    float azimuth, elevation, distance;
};
struct Source {
    const std::vector<float>* pcm;
    unsigned channels, channel;
    unsigned long long start;
    float buffer[1024];
};

static OSStatus
pull(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* list) {
    auto& source = *static_cast<Source*>(context);
    if (frames > 1024 || list->mNumberBuffers != 1)
        return kAudio_ParamError;
    for (unsigned i = 0; i < frames; ++i)
        source.buffer[i] = (*source.pcm)[(source.start + i) * source.channels + source.channel];
    list->mBuffers[0] = {1, static_cast<UInt32>(frames * sizeof(float)), source.buffer};
    return noErr;
}

static bool checked(OSStatus value, const char* stage) {
    if (value)
        std::fprintf(stderr, "%s: %d\n", stage, static_cast<int>(value));
    return value == noErr;
}

static AudioStreamBasicDescription format(unsigned channels) {
    AudioStreamBasicDescription value{};
    value.mSampleRate = 48000;
    value.mFormatID = kAudioFormatLinearPCM;
    value.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    value.mBytesPerFrame = value.mBytesPerPacket = sizeof(float);
    value.mFramesPerPacket = 1;
    value.mChannelsPerFrame = channels;
    value.mBitsPerChannel = 32;
    return value;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: render_decoded PCM.f32 POSITIONS.csv OBJECTS OUTPUT.f32\n");
        return 2;
    }
    if (std::filesystem::exists(argv[4]))
        return 2;
    const unsigned channels = std::strtoul(argv[3], nullptr, 10);
    if (!channels || channels > 24)
        return 2;
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || static_cast<unsigned long long>(input.tellg()) % (channels * sizeof(float)))
        return 2;
    std::vector<float> pcm(static_cast<std::size_t>(input.tellg()) / sizeof(float));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(pcm.data()), pcm.size() * sizeof(float)))
        return 2;
    std::vector<Position> positions;
    std::ifstream trajectory(argv[2]);
    std::string line;
    while (std::getline(trajectory, line)) {
        Position p{};
        if (std::sscanf(
                line.c_str(), "%llu,%u,%f,%f,%f", &p.sample, &p.object, &p.azimuth, &p.elevation, &p.distance) != 5 ||
            p.object >= channels)
            return 2;
        positions.push_back(p);
    }
    if (positions.empty())
        return 2;
    AudioComponentDescription desc{
        kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer, kAudioUnitManufacturer_Apple, 0, 0};
    AudioUnit unit = nullptr;
    auto component = AudioComponentFindNext(nullptr, &desc);
    if (!component || !checked(AudioComponentInstanceNew(component, &unit), "create"))
        return 1;
    auto property = [&](AudioUnitPropertyID key, AudioUnitScope scope, UInt32 bus, UInt32 value) {
        return checked(AudioUnitSetProperty(unit, key, scope, bus, &value, sizeof(value)), "property");
    };
    if (!property(
            kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, kSpatialMixerOutputType_Headphones) ||
        !property(kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, channels) ||
        !property(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, 1024))
        return 1;
    auto stereo = format(2), mono = format(1);
    if (!checked(AudioUnitSetProperty(
                     unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &stereo, sizeof(stereo)),
                 "output_format"))
        return 1;
    std::vector<Source> sources(channels);
    for (unsigned i = 0; i < channels; ++i) {
        sources[i] = Source{&pcm, channels, i, 0, {}};
        AURenderCallbackStruct callback{pull, &sources[i]};
        if (!checked(AudioUnitSetProperty(
                         unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i, &mono, sizeof(mono)),
                     "input_format") ||
            !property(kAudioUnitProperty_SpatializationAlgorithm,
                      kAudioUnitScope_Input,
                      i,
                      kSpatializationAlgorithm_HRTFHQ) ||
            !property(kAudioUnitProperty_SpatialMixerSourceMode,
                      kAudioUnitScope_Input,
                      i,
                      kSpatialMixerSourceMode_PointSource) ||
            !checked(
                AudioUnitSetProperty(
                    unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, i, &callback, sizeof(callback)),
                "callback"))
            return 1;
    }
    if (!checked(AudioUnitInitialize(unit), "initialize"))
        return 1;
    std::ofstream output(argv[4], std::ios::binary);
    if (!output)
        return 2;
    std::size_t cursor = 0;
    const auto total = pcm.size() / channels;
    for (std::size_t start = 0; start < total; start += 1024) {
        while (cursor < positions.size() && positions[cursor].sample <= start) {
            const auto& p = positions[cursor++];
            // ADM/APAC spherical azimuth is +left; SpatialMixer azimuth is +right.
            for (auto pair : {std::pair{kSpatialMixerParam_Azimuth, -p.azimuth},
                              std::pair{kSpatialMixerParam_Elevation, p.elevation},
                              std::pair{kSpatialMixerParam_Distance, p.distance},
                              std::pair{kSpatialMixerParam_Gain, 0.0F}}) {
                if (!checked(AudioUnitSetParameter(unit, pair.first, kAudioUnitScope_Input, p.object, pair.second, 0),
                             "position"))
                    return 1;
            }
        }
        for (auto& source : sources)
            source.start = start;
        const UInt32 frames = std::min<std::size_t>(1024, total - start);
        float left[1024]{}, right[1024]{};
        const auto bytes = static_cast<UInt32>(frames * sizeof(float));
        struct {
            UInt32 count;
            AudioBuffer buffers[2];
        } buffers{2, {{1, bytes, left}, {1, bytes, right}}};
        AudioTimeStamp time{};
        time.mSampleTime = start;
        time.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags = 0;
        if (!checked(AudioUnitRender(unit, &flags, &time, 0, frames, reinterpret_cast<AudioBufferList*>(&buffers)),
                     "render"))
            return 1;
        double l = 0, r = 0;
        for (UInt32 i = 0; i < frames; ++i) {
            float pair[2]{left[i], right[i]};
            output.write(reinterpret_cast<const char*>(pair), sizeof(pair));
            l += double(left[i]) * left[i];
            r += double(right[i]) * right[i];
        }
        std::printf("{\"sample\":%zu,\"frames\":%u,\"left_rms\":%.10g,\"right_rms\":%.10g}\n",
                    start,
                    frames,
                    std::sqrt(l / frames),
                    std::sqrt(r / frames));
    }
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return output.good() ? 0 : 1;
}
