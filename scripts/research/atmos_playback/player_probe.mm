// Real-time AVPlayer / MTAudioProcessingTap research probe. No offline rendering.
// The tap forwards source PCM unchanged and copies it to bounded memory.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#import <MediaToolbox/MediaToolbox.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void emit(NSDictionary* event) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:event options:0 error:nil];
    if (data) {
        fwrite(data.bytes, 1, data.length, stdout);
        putchar('\n');
        fflush(stdout);
    }
}

static NSDictionary* describe(const AudioStreamBasicDescription& format) {
    char fourcc[5] = {char(format.mFormatID >> 24), char(format.mFormatID >> 16),
                      char(format.mFormatID >> 8), char(format.mFormatID), 0};
    return @{@"sample_rate": @(format.mSampleRate), @"format": @(fourcc),
             @"flags": @(format.mFormatFlags), @"channels": @(format.mChannelsPerFrame),
             @"bits": @(format.mBitsPerChannel), @"bytes_per_frame": @(format.mBytesPerFrame)};
}

struct Block {
    CMTimeRange range{};
    size_t offset{};
    CMItemCount requested{}, frames{};
    UInt32 buffers{}, flags{}, channels{};
    OSStatus status{};
};

struct TapState {
    static constexpr size_t max_samples = 48000 * 10 * 32;
    std::vector<float> pcm = std::vector<float>(max_samples);
    Block blocks[4096]{};
    AudioStreamBasicDescription formats[16]{};
    AudioStreamBasicDescription current{};
    size_t samples{}, block_count{}, prepare_count{}, rejected{}, overflow{};
    std::atomic<bool> enabled{true};
    std::atomic<unsigned> in_flight{0};
};

static void tap_init(MTAudioProcessingTapRef, void* info, void** storage) { *storage = info; }
static void tap_finalize(MTAudioProcessingTapRef) {}
static void tap_unprepare(MTAudioProcessingTapRef) {}
static void tap_prepare(MTAudioProcessingTapRef tap, CMItemCount, const AudioStreamBasicDescription* format) {
    auto& state = *static_cast<TapState*>(MTAudioProcessingTapGetStorage(tap));
    state.current = *format;
    if (state.prepare_count < 16)
        state.formats[state.prepare_count++] = *format;
}

static void tap_process(MTAudioProcessingTapRef tap, CMItemCount requested, MTAudioProcessingTapFlags,
                        AudioBufferList* buffers, CMItemCount* frames, MTAudioProcessingTapFlags* flags) {
    auto& state = *static_cast<TapState*>(MTAudioProcessingTapGetStorage(tap));
    state.in_flight.fetch_add(1, std::memory_order_acq_rel);
    CMTimeRange range = kCMTimeRangeInvalid;
    *frames = 0;
    *flags = 0;
    const auto status = MTAudioProcessingTapGetSourceAudio(tap, requested, buffers, flags, &range, frames);
    if (state.enabled.load(std::memory_order_acquire)) {
        const auto channels = state.current.mChannelsPerFrame;
        if (state.block_count < 4096)
            state.blocks[state.block_count++] = {range, state.samples, requested, *frames,
                                                 buffers->mNumberBuffers, *flags, channels, status};
        if (status == noErr && *frames > 0) {
            bool valid = channels > 0 && channels <= 32 && state.current.mFormatID == kAudioFormatLinearPCM &&
                         state.current.mBitsPerChannel == 32 &&
                         (state.current.mFormatFlags & kAudioFormatFlagIsFloat) &&
                         !(state.current.mFormatFlags & kAudioFormatFlagIsBigEndian);
            UInt32 found = 0;
            for (UInt32 b = 0; b < buffers->mNumberBuffers; ++b) {
                const auto& buffer = buffers->mBuffers[b];
                found += buffer.mNumberChannels;
                valid = valid && buffer.mData && buffer.mDataByteSize >=
                    size_t(*frames) * buffer.mNumberChannels * sizeof(float);
            }
            valid = valid && found == channels;
            if (!valid) {
                ++state.rejected;
            } else if (size_t(*frames) * channels > state.pcm.size() - state.samples) {
                ++state.overflow;
            } else {
                UInt32 channel = 0;
                for (UInt32 b = 0; b < buffers->mNumberBuffers; ++b) {
                    const auto& buffer = buffers->mBuffers[b];
                    const auto* source = static_cast<const float*>(buffer.mData);
                    for (CMItemCount i = 0; i < *frames; ++i)
                        for (UInt32 c = 0; c < buffer.mNumberChannels; ++c)
                            state.pcm[state.samples + size_t(i) * channels + channel + c] =
                                source[size_t(i) * buffer.mNumberChannels + c];
                    channel += buffer.mNumberChannels;
                }
                state.samples += size_t(*frames) * channels;
            }
        }
    }
    state.in_flight.fetch_sub(1, std::memory_order_release);
}

static id seconds(CMTime time) {
    double value = CMTimeGetSeconds(time);
    return std::isfinite(value) ? (id)@(value) : (id)[NSNull null];
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr, "usage: player_probe MEDIA OUTPUT_DIRECTORY none|pre|post START DURATION\n");
            return 2;
        }
        NSString* directory = @(argv[2]);
        const bool tapped = strcmp(argv[3], "none") != 0;
        if (tapped && strcmp(argv[3], "pre") && strcmp(argv[3], "post"))
            return 2;
        const double start = strtod(argv[4], nullptr), duration = strtod(argv[5], nullptr);
        if (start < 0 || duration <= 0 || duration > 8)
            return 2;
        NSError* error = nil;
        if ([[NSFileManager defaultManager] fileExistsAtPath:directory] ||
            ![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
                                                       attributes:nil error:&error])
            return 2;
        auto* state = tapped ? new TapState : nullptr; // Process lifetime; tap teardown can be asynchronous.
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
        NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        if (tracks.count != 1)
            return 2;
        NSMutableArray* source_formats = [NSMutableArray array];
        for (id value in tracks[0].formatDescriptions) {
            auto description = (__bridge CMAudioFormatDescriptionRef)value;
            const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description);
            size_t size = 0;
            const auto* layout = CMAudioFormatDescriptionGetChannelLayout(description, &size);
            NSMutableDictionary* record = [describe(*asbd) mutableCopy];
            if (layout && size >= offsetof(AudioChannelLayout, mChannelDescriptions)) {
                record[@"layout_tag"] = @(layout->mChannelLayoutTag);
                record[@"layout_bitmap"] = @(layout->mChannelBitmap);
                record[@"layout_descriptions"] = @(layout->mNumberChannelDescriptions);
            }
            [source_formats addObject:record];
        }
        emit(@{@"stage": @"asset", @"path": @(argv[1]), @"duration": seconds(asset.duration),
               @"formats": source_formats});
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                           kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioDeviceID device = 0;
        UInt32 size = sizeof(device);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device);
        address.mSelector = kAudioObjectPropertyName;
        CFStringRef name = nullptr;
        size = sizeof(name);
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) == noErr && name) {
            emit(@{@"stage": @"output_device", @"id": @(device), @"name": (__bridge NSString*)name});
            CFRelease(name);
        }
        AVPlayerItem* item = [AVPlayerItem playerItemWithAsset:asset];
        MTAudioProcessingTapRef tap = nullptr;
        if (tapped) {
            MTAudioProcessingTapCallbacks callbacks{kMTAudioProcessingTapCallbacksVersion_0, state, tap_init,
                tap_finalize, tap_prepare, tap_unprepare, tap_process};
            auto creation_flags = !strcmp(argv[3], "pre") ? kMTAudioProcessingTapCreationFlag_PreEffects :
                                                           kMTAudioProcessingTapCreationFlag_PostEffects;
            const auto status = MTAudioProcessingTapCreate(kCFAllocatorDefault, &callbacks, creation_flags, &tap);
            emit(@{@"stage": @"tap_create", @"status": @(status), @"mode": @(argv[3])});
            if (status != noErr)
                return 1;
            AVMutableAudioMixInputParameters* parameters =
                [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:tracks[0]];
            parameters.audioTapProcessor = tap;
            AVMutableAudioMix* mix = [AVMutableAudioMix audioMix];
            mix.inputParameters = @[parameters];
            item.audioMix = mix;
        }
        AVPlayer* player = [AVPlayer playerWithPlayerItem:item];
        const bool audible = getenv("ATMOS_PROBE_AUDIBLE") != nullptr;
        // Muting can make AVPlayer stop supplying nonzero source PCM after its
        // initial prefetch. Use normal audible playback for quantitative captures.
        player.muted = !audible;
        __block BOOL ended = NO;
        id observer = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
            object:item queue:nil usingBlock:^(NSNotification*) { ended = YES; }];
        emit(@{@"stage": @"player_created", @"tap_mode": @(argv[3]), @"start": @(start), @"duration": @(duration),
               @"muted": @(!audible), @"spatial_formats": @(item.allowedAudioSpatializationFormats), @"pid": @(getpid())});
        item.forwardPlaybackEndTime = CMTimeMakeWithSeconds(start + duration, 48000);
        [player seekToTime:CMTimeMakeWithSeconds(start, 48000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
            completionHandler:^(BOOL finished) { if (finished) [player play]; }];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:duration + 25];
        while (!ended && deadline.timeIntervalSinceNow > 0 && item.status != AVPlayerItemStatusFailed)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
        [player pause];
        emit(@{@"stage": @"player_finished", @"ended": @(ended), @"status": @(item.status),
               @"time": seconds(item.currentTime), @"error": item.error.description ?: @""});
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        if (state)
            state->enabled.store(false, std::memory_order_release);
        [player replaceCurrentItemWithPlayerItem:nil];
        if (state) {
            while (state->in_flight.load(std::memory_order_acquire))
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
            NSMutableArray* formats = [NSMutableArray array];
            for (size_t i = 0; i < state->prepare_count; ++i)
                [formats addObject:describe(state->formats[i])];
            NSMutableArray* blocks = [NSMutableArray array];
            for (size_t i = 0; i < state->block_count; ++i) {
                const auto& block = state->blocks[i];
                [blocks addObject:@{@"start": seconds(block.range.start), @"duration": seconds(block.range.duration),
                    @"sample_offset": @(block.offset), @"requested": @(block.requested), @"frames": @(block.frames),
                    @"buffers": @(block.buffers), @"channels": @(block.channels), @"flags": @(block.flags),
                    @"status": @(block.status)}];
            }
            NSDictionary* report = @{@"mode": @(argv[3]), @"formats": formats, @"samples": @(state->samples),
                @"rejected_blocks": @(state->rejected), @"overflow_blocks": @(state->overflow), @"blocks": blocks,
                @"observation": @"Real-time AVPlayer tap; source buffers forwarded unchanged; packed float32 copy"};
            NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
            if (!json || ![json writeToFile:[directory stringByAppendingPathComponent:@"tap.json"]
                                  options:NSDataWritingWithoutOverwriting error:&error])
                return 1;
            FILE* file = fopen([directory stringByAppendingPathComponent:@"tap.f32"].fileSystemRepresentation, "wbx");
            if (!file)
                return 1;
            const auto written = fwrite(state->pcm.data(), sizeof(float), state->samples, file);
            fclose(file);
            emit(@{@"stage": @"tap_finished", @"formats": formats, @"samples": @(state->samples),
                   @"rejected": @(state->rejected), @"overflow": @(state->overflow), @"blocks": @(state->block_count)});
            CFRelease(tap);
            if (written != state->samples || !state->samples || state->rejected || state->overflow)
                return 1;
        }
        return ended && item.status == AVPlayerItemStatusReadyToPlay ? 0 : 1;
    }
}
