// AVFoundation readback is a compatibility check, not an object-rendering test.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#include <math.h>
#include <stdlib.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 && argc != 3 && argc != 4 && argc != 6) {
            fprintf(stderr, "usage: read_asset INPUT [OUTPUT.f32 [CHANNELS [START_SECONDS DURATION_SECONDS]]]\n");
            return 2;
        }
        FILE *pcm_file = NULL;
        if (argc >= 3) {
            pcm_file = fopen(argv[2], "wbx");
            if (!pcm_file)
                return 2;
        }
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        if (tracks.count != 1)
            return 1;
        NSError *error = nil;
        AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!reader)
            return 1;
        NSMutableDictionary *settings = [@{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVLinearPCMIsFloatKey : @YES,
            AVLinearPCMBitDepthKey : @32,
            AVLinearPCMIsNonInterleaved : @NO
        } mutableCopy];
        if (argc >= 4) {
            const unsigned requested_channels = (unsigned)strtoul(argv[3], NULL, 10);
            if (requested_channels > 256)
                return 2;
            if (requested_channels)
                settings[AVNumberOfChannelsKey] = @(requested_channels);
        }
        if (argc == 6) {
            const double start = strtod(argv[4], NULL), duration = strtod(argv[5], NULL);
            if (!isfinite(start) || !isfinite(duration) || start < 0 || duration <= 0)
                return 2;
            reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(start, 48000),
                                              CMTimeMakeWithSeconds(duration, 48000));
        }
        AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc] initWithTrack:tracks[0]
                                                                            outputSettings:settings];
        [reader addOutput:output];
        if (![reader startReading])
            return 1;
        long long frames = 0, values = 0;
        unsigned channels = 0;
        double energy = 0;
        CMSampleBufferRef sample = NULL;
        while ((sample = [output copyNextSampleBuffer])) {
            const AudioStreamBasicDescription *format =
                CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
            if (!format || format->mBitsPerChannel != 32 || !(format->mFormatFlags & kAudioFormatFlagIsFloat)) {
                CFRelease(sample);
                return 1;
            }
            channels = format->mChannelsPerFrame;
            if (!frames) {
                size_t layout_size = 0;
                const AudioChannelLayout *layout = CMAudioFormatDescriptionGetChannelLayout(
                    CMSampleBufferGetFormatDescription(sample), &layout_size);
                printf("{\"stage\":\"asset_layout\",\"channels\":%u,\"tag\":%u,\"labels\":[",
                       channels, layout ? layout->mChannelLayoutTag : 0);
                if (layout && layout_size >= 12) {
                    for (UInt32 i = 0; i < layout->mNumberChannelDescriptions && 12U + 20U * (i + 1U) <= layout_size; ++i)
                        printf("%s%u", i ? "," : "", layout->mChannelDescriptions[i].mChannelLabel);
                }
                puts("]}");
            }
            frames += CMSampleBufferGetNumSamples(sample);
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
            const size_t bytes = block ? CMBlockBufferGetDataLength(block) : 0;
            float *data = bytes ? malloc(bytes) : NULL;
            if (!data || bytes % sizeof(float) || CMBlockBufferCopyDataBytes(block, 0, bytes, data)) {
                free(data);
                CFRelease(sample);
                return 1;
            }
            for (size_t i = 0; i < bytes / sizeof(float); ++i) {
                energy += (double)data[i] * data[i];
                ++values;
            }
            if (pcm_file && fwrite(data, 1, bytes, pcm_file) != bytes) {
                free(data);
                CFRelease(sample);
                fclose(pcm_file);
                return 1;
            }
            free(data);
            CFRelease(sample);
        }
        printf(
            "{\"stage\":\"asset_read\",\"status\":%ld,\"channels\":%u,\"frames\":%lld,\"values\":%lld,\"rms\":%.12g}\n",
            (long)reader.status, channels, frames, values, values ? sqrt(energy / values) : 0);
        if (reader.error)
            NSLog(@"%@", reader.error);
        if (pcm_file && fclose(pcm_file))
            return 1;
        return reader.status == AVAssetReaderStatusCompleted && values > 0 ? 0 : 1;
    }
}
