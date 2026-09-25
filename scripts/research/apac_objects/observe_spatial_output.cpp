// Diagnostic observer for a self-owned AVPlayer only. Forward every system call
// unchanged, then copy rendered stereo PCM to bounded memory. No playback fix,
// decoder patch, format override, gain change, or sample modification is applied.
#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr unsigned max_units = 12;
constexpr unsigned max_frames = 48000 * 8;
struct Unit {
    AudioUnit handle{};
    double rate{};
    UInt32 format{}, flags{}, channels{};
    bool spatial{};
    float* pcm{};
    std::atomic<unsigned> frames{};
    unsigned calls{};
};
Unit units[max_units];
unsigned count = 0;
const char* destination = nullptr;
using SetProperty = decltype(&AudioUnitSetProperty);
using Render = decltype(&AudioUnitRender);
SetProperty real_set = nullptr;
Render real_render = nullptr;

Unit* find(AudioUnit handle, bool add) {
    for (unsigned i = 0; i < count; ++i)
        if (units[i].handle == handle)
            return &units[i];
    if (!add || count >= max_units)
        return nullptr;
    units[count].handle = handle;
    return &units[count++];
}

void finish() {
    if (!destination)
        return;
    for (unsigned i = 0; i < count; ++i) {
        const auto& unit = units[i];
        const unsigned frames = unit.frames.load(std::memory_order_acquire);
        if (!unit.spatial || !frames)
            continue;
        char path[4096];
        std::snprintf(path, sizeof(path), "%s-unit%u.f32", destination, i);
        FILE* file = std::fopen(path, "wbx");
        if (file) {
            std::fwrite(unit.pcm, sizeof(float) * 2, frames, file);
            std::fclose(file);
        }
        std::snprintf(path, sizeof(path), "%s-unit%u.json", destination, i);
        file = std::fopen(path, "wx");
        if (file) {
            std::fprintf(file, "{\"sample_rate\":%.0f,\"frames\":%u,\"channels\":2,\"calls\":%u,"
                               "\"observation\":\"copy after original AudioUnitRender; all arguments unchanged\"}\n",
                         unit.rate, frames, unit.calls);
            std::fclose(file);
        }
    }
}

__attribute__((constructor)) void start() {
    // dyld excludes references made by the interposing image itself. A dlsym
    // lookup would be interposed again and recurse instead of forwarding.
    real_set = &AudioUnitSetProperty;
    real_render = &AudioUnitRender;
    destination = std::getenv("APAC_SPATIAL_OBSERVER");
    if (destination && real_set && real_render) {
        for (auto& unit : units)
            unit.pcm = static_cast<float*>(std::calloc(max_frames * 2, sizeof(float)));
        std::atexit(finish);
    }
}

OSStatus observe_set(AudioUnit handle, AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
                     const void* data, UInt32 size) {
    const auto result = real_set(handle, property, scope, element, data, size);
    if (destination && !result && (property == 3231 || (property == 8 && scope == kAudioUnitScope_Output))) {
        if (auto* unit = find(handle, true)) {
            if (property == 3231)
                unit->spatial = true;
            if (property == 8 && size == sizeof(AudioStreamBasicDescription)) {
                const auto& asbd = *static_cast<const AudioStreamBasicDescription*>(data);
                unit->rate = asbd.mSampleRate;
                unit->format = asbd.mFormatID;
                unit->flags = asbd.mFormatFlags;
                unit->channels = asbd.mChannelsPerFrame;
            }
        }
    }
    return result;
}

OSStatus observe_render(AudioUnit handle, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                        UInt32 bus, UInt32 frames, AudioBufferList* buffers) {
    const auto result = real_render(handle, flags, time, bus, frames, buffers);
    if (destination && !result) {
        auto* unit = find(handle, false);
        if (!unit || !unit->pcm || !unit->spatial || unit->channels != 2 || unit->format != kAudioFormatLinearPCM ||
            !(unit->flags & kAudioFormatFlagIsFloat))
            return result;
        const unsigned first = unit->frames.load(std::memory_order_relaxed);
        if (frames > max_frames - first)
            return result;
        auto* target = unit->pcm + 2 * first;
        if (buffers->mNumberBuffers == 2 && buffers->mBuffers[0].mNumberChannels == 1 &&
            buffers->mBuffers[1].mNumberChannels == 1 && buffers->mBuffers[0].mDataByteSize >= frames * 4 &&
            buffers->mBuffers[1].mDataByteSize >= frames * 4) {
            const auto* left = static_cast<const float*>(buffers->mBuffers[0].mData);
            const auto* right = static_cast<const float*>(buffers->mBuffers[1].mData);
            for (unsigned i = 0; i < frames; ++i) {
                target[2 * i] = left[i];
                target[2 * i + 1] = right[i];
            }
        } else if (buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mNumberChannels == 2 &&
                   buffers->mBuffers[0].mDataByteSize >= frames * 8) {
            std::memcpy(target, buffers->mBuffers[0].mData, frames * 8);
        } else {
            return result;
        }
        ++unit->calls;
        unit->frames.store(first + frames, std::memory_order_release);
    }
    return result;
}

__attribute__((used, section("__DATA,__interpose"))) const struct {
    const void* observer;
    const void* original;
} interpose[] = {{reinterpret_cast<const void*>(observe_set), reinterpret_cast<const void*>(AudioUnitSetProperty)},
                 {reinterpret_cast<const void*>(observe_render), reinterpret_cast<const void*>(AudioUnitRender)}};
} // namespace
