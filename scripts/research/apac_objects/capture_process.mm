// Capture only one selected process's existing output-device stream. No mute,
// route, gain, player setting, or decoder change. Tap/aggregate are private and
// destroyed on exit; no microphone or other applications are included.
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Capture {
    std::vector<float> pcm;
    std::atomic<size_t> frames{0};
    UInt32 channels = 0;
    AudioStreamBasicDescription format{};
    std::atomic<unsigned> calls{0}, empty{0}, invalid{0};
    UInt32 first_buffers = 0, first_channels = 0, first_bytes = 0;
};

static OSStatus receive(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                        const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* context) {
    auto& capture = *static_cast<Capture*>(context);
    const auto call = capture.calls.fetch_add(1, std::memory_order_relaxed);
    if (call == 0 && input) {
        capture.first_buffers = input->mNumberBuffers;
        if (input->mNumberBuffers) {
            capture.first_channels = input->mBuffers[0].mNumberChannels;
            capture.first_bytes = input->mBuffers[0].mDataByteSize;
        }
    }
    if (!input || !input->mNumberBuffers) {
        capture.empty.fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }
    const auto& first = input->mBuffers[0];
    if (!first.mNumberChannels || !first.mData) {
        capture.empty.fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }
    size_t frames = first.mDataByteSize / (4 * first.mNumberChannels);
    const auto offset = capture.frames.load(std::memory_order_relaxed);
    frames = std::min(frames, capture.pcm.size() / capture.channels - offset);
    UInt32 channel = 0;
    for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
        const auto& buffer = input->mBuffers[b];
        if (!buffer.mData || buffer.mDataByteSize < frames * 4 * buffer.mNumberChannels ||
            channel + buffer.mNumberChannels > capture.channels) {
            capture.invalid.fetch_add(1, std::memory_order_relaxed);
            return noErr;
        }
        const auto* data = static_cast<const float*>(buffer.mData);
        for (size_t i = 0; i < frames; ++i)
            for (UInt32 c = 0; c < buffer.mNumberChannels; ++c)
                capture.pcm[(offset + i) * capture.channels + channel + c] = data[i * buffer.mNumberChannels + c];
        channel += buffer.mNumberChannels;
    }
    if (channel == capture.channels)
        capture.frames.store(offset + frames, std::memory_order_release);
    return noErr;
}

struct Resources {
    AudioObjectID tap = 0, aggregate = 0;
    AudioDeviceIOProcID callback = nullptr;
    ~Resources() {
        if (aggregate && callback) {
            AudioDeviceStop(aggregate, callback);
            AudioDeviceDestroyIOProcID(aggregate, callback);
        }
        if (aggregate)
            AudioHardwareDestroyAggregateDevice(aggregate);
        if (tap)
            AudioHardwareDestroyProcessTap(tap);
    }
};

static bool check(const char* stage, OSStatus value) {
    std::printf("{\"stage\":\"%s\",\"status\":%d}\n", stage, value);
    std::fflush(stdout);
    return value == noErr;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 4)
            return 2;
        pid_t pid = std::strtol(argv[1], nullptr, 10);
        const double duration = std::strtod(argv[2], nullptr);
        if (pid <= 0 || duration <= 0 || duration > 30)
            return 2;
        FILE* output = std::fopen(argv[3], "wbx");
        if (!output)
            return 2;
        AudioObjectID process = 0, device = 0;
        UInt32 size = sizeof(process);
        AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslatePIDToProcessObject,
                                            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (!check("process", AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(pid), &pid, &size, &process)))
            return 1;
        address.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        size = sizeof(device);
        if (!check("device", AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device)))
            return 1;
        address.mSelector = kAudioDevicePropertyDeviceUID;
        CFStringRef device_uid = nullptr;
        size = sizeof(device_uid);
        if (!check("device_uid", AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &device_uid)))
            return 1;
        // Use the existing output-only device as the private aggregate clock.
        // Refuse an input-capable subdevice so this never includes a microphone.
        address.mSelector = kAudioDevicePropertyStreams;
        address.mScope = kAudioObjectPropertyScopeInput;
        UInt32 input_stream_bytes = 0;
        if (!check("output_only_clock", AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &input_stream_bytes)) ||
            input_stream_bytes != 0)
            return 1;
        address.mScope = kAudioObjectPropertyScopeGlobal;
        NSString* clock_uid = CFBridgingRelease(device_uid);
        const bool mixdown = std::getenv("APAC_TAP_MIXDOWN") != nullptr;
        CATapDescription* description = mixdown
            ? [[CATapDescription alloc] initStereoMixdownOfProcesses:@[@(process)]]
            : [[CATapDescription alloc] initWithProcesses:@[@(process)] andDeviceUID:clock_uid withStream:0];
        description.name = @"APAC single-process diagnostic";
        description.UUID = [NSUUID UUID];
        description.privateTap = YES;
        description.exclusive = NO;
        description.muteBehavior = CATapUnmuted;
        Resources resources;
        if (!check("create_tap", AudioHardwareCreateProcessTap(description, &resources.tap)))
            return 1;
        CFStringRef tap_uid_ref = nullptr;
        address.mSelector = kAudioTapPropertyUID;
        size = sizeof(tap_uid_ref);
        if (!check("tap_uid", AudioObjectGetPropertyData(resources.tap, &address, 0, nullptr, &size, &tap_uid_ref)))
            return 1;
        NSString* tap_uid = CFBridgingRelease(tap_uid_ref);
        Capture capture;
        address.mSelector = kAudioTapPropertyFormat;
        size = sizeof(capture.format);
        if (!check("tap_format", AudioObjectGetPropertyData(resources.tap, &address, 0, nullptr, &size, &capture.format)))
            return 1;
        if (capture.format.mFormatID != kAudioFormatLinearPCM || capture.format.mBitsPerChannel != 32 ||
            !(capture.format.mFormatFlags & kAudioFormatFlagIsFloat) || !capture.format.mChannelsPerFrame)
            return 1;
        capture.channels = capture.format.mChannelsPerFrame;
        capture.pcm.resize(static_cast<size_t>(capture.format.mSampleRate * (duration + 1)) * capture.channels);
        NSDictionary* aggregate = @{@kAudioAggregateDeviceNameKey: @"APAC capture-only aggregate",
            @kAudioAggregateDeviceUIDKey: [NSUUID UUID].UUIDString, @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceMainSubDeviceKey: clock_uid,
            @kAudioAggregateDeviceSubDeviceListKey: @[@{@kAudioSubDeviceUIDKey: clock_uid}],
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceTapListKey: @[@{@kAudioSubTapUIDKey: tap_uid,
                                                  @kAudioSubTapDriftCompensationKey: @YES}]};
        if (!check("create_aggregate", AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &resources.aggregate)))
            return 1;
        address.mSelector = kAudioDevicePropertyStreamConfiguration;
        address.mScope = kAudioObjectPropertyScopeInput;
        UInt32 configuration_size = 0;
        if (check("aggregate_input_size", AudioObjectGetPropertyDataSize(resources.aggregate, &address, 0, nullptr, &configuration_size))) {
            std::vector<unsigned char> configuration(configuration_size);
            if (check("aggregate_input", AudioObjectGetPropertyData(resources.aggregate, &address, 0, nullptr, &configuration_size, configuration.data()))) {
                const auto* list = reinterpret_cast<const AudioBufferList*>(configuration.data());
                UInt32 channels = 0;
                for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
                    channels += list->mBuffers[i].mNumberChannels;
                std::printf("{\"stage\":\"aggregate_format\",\"buffers\":%u,\"channels\":%u,\"mixdown\":%s}\n",
                            list->mNumberBuffers, channels, mixdown ? "true" : "false");
            }
        }
        if (!check("callback", AudioDeviceCreateIOProcID(resources.aggregate, receive, &capture, &resources.callback)) ||
            !check("start", AudioDeviceStart(resources.aggregate, resources.callback)))
            return 1;
        std::printf("{\"stage\":\"ready\",\"pid\":%d,\"channels\":%u,\"sample_rate\":%.0f,\"seconds\":%.1f}\n",
                    pid, capture.channels, capture.format.mSampleRate, duration);
        std::fflush(stdout);
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:duration];
        while (deadline.timeIntervalSinceNow > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
        AudioDeviceStop(resources.aggregate, resources.callback);
        const auto frames = capture.frames.load(std::memory_order_acquire);
        std::fwrite(capture.pcm.data(), sizeof(float) * capture.channels, frames, output);
        std::fclose(output);
        std::printf("{\"stage\":\"captured\",\"frames\":%zu,\"channels\":%u,\"sample_rate\":%.0f,"
                    "\"calls\":%u,\"empty\":%u,\"invalid\":%u,\"first_buffers\":%u,\"first_channels\":%u,\"first_bytes\":%u}\n",
                    frames, capture.channels, capture.format.mSampleRate, capture.calls.load(), capture.empty.load(),
                    capture.invalid.load(), capture.first_buffers, capture.first_channels, capture.first_bytes);
        return frames ? 0 : 1;
    }
}
