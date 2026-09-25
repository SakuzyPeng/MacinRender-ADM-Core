// Observe the stock AVPlayer pipeline in a self-owned process. Muted for tracing.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

static void emit(NSDictionary *event) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:event options:0 error:nil];
    if (data) {
        fwrite(data.bytes, 1, data.length, stdout);
        putchar('\n');
        fflush(stdout);
    }
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3 && argc != 5)
            return 2;
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        if (tracks.count != 1)
            return 2;
        NSMutableArray *formats = [NSMutableArray array];
        for (id value in tracks[0].formatDescriptions) {
            CMAudioFormatDescriptionRef description = (__bridge CMAudioFormatDescriptionRef)value;
            const AudioStreamBasicDescription *asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description);
            CFDictionaryRef extensions = CMFormatDescriptionGetExtensions(description);
            if (extensions)
                [formats addObject:(__bridge NSDictionary *)extensions];
            if (asbd)
                emit(@{@"stage": @"asset_format", @"format": @(asbd->mFormatID),
                       @"channels": @(asbd->mChannelsPerFrame), @"flags": @(asbd->mFormatFlags)});
        }
        NSError *error = nil;
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:formats
                                                                  format:NSPropertyListXMLFormat_v1_0
                                                                 options:0 error:&error];
        if (!plist || ![plist writeToFile:[NSString stringWithUTF8String:argv[2]]
                                 options:NSDataWritingWithoutOverwriting error:&error])
            return 2;
        AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice,
                                              kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioDeviceID device = 0;
        UInt32 bytes = sizeof(device);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &bytes, &device);
        CFStringRef name = NULL;
        bytes = sizeof(name);
        address.mSelector = kAudioObjectPropertyName;
        if (AudioObjectGetPropertyData(device, &address, 0, NULL, &bytes, &name) == noErr && name) {
            emit(@{@"stage": @"output_device", @"name": (__bridge NSString *)name, @"id": @(device)});
            CFRelease(name);
        }
        AVAsset *playback_asset = asset;
        if (getenv("APAC_PROBE_COMPOSITION")) {
            AVMutableComposition *composition = [AVMutableComposition composition];
            AVMutableCompositionTrack *track = [composition addMutableTrackWithMediaType:AVMediaTypeAudio
                                                                        preferredTrackID:kCMPersistentTrackID_Invalid];
            if (![track insertTimeRange:CMTimeRangeMake(kCMTimeZero, asset.duration) ofTrack:tracks[0]
                                 atTime:kCMTimeZero error:&error])
                return 2;
            playback_asset = composition;
        }
        AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:playback_asset];
        if (getenv("APAC_PROBE_UNITY_MIX")) {
            AVAssetTrack *track = [playback_asset tracksWithMediaType:AVMediaTypeAudio][0];
            AVMutableAudioMixInputParameters *parameters = [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:track];
            [parameters setVolume:1 atTime:kCMTimeZero];
            AVMutableAudioMix *mix = [AVMutableAudioMix audioMix];
            mix.inputParameters = @[parameters];
            item.audioMix = mix;
        }
        AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
        // Silence only this diagnostic player; leave the system output and
        // metadata/spatial processing configuration at their default settings.
        player.muted = YES;
        __block BOOL ended = NO;
        id observer = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                                                       object:item queue:nil
                                                                   usingBlock:^(NSNotification *note) {
            (void)note;
            ended = YES;
        }];
        emit(@{@"stage": @"player_created", @"playable": @(asset.playable),
               @"decodable": @(tracks[0].decodable), @"spatial_formats": @(item.allowedAudioSpatializationFormats),
               @"muted_for_observation": @YES, @"composition_probe": @(getenv("APAC_PROBE_COMPOSITION") != NULL),
               @"unity_mix_probe": @(getenv("APAC_PROBE_UNITY_MIX") != NULL)});
        if (argc == 5) {
            const double start = strtod(argv[3], NULL), duration = strtod(argv[4], NULL);
            if (start < 0 || duration <= 0)
                return 2;
            item.forwardPlaybackEndTime = CMTimeMakeWithSeconds(start + duration, 48000);
            [player seekToTime:CMTimeMakeWithSeconds(start, 48000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
             completionHandler:^(BOOL finished) {
                if (finished)
                    [player play];
            }];
        } else {
            [player play];
        }
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:20];
        while (!ended && deadline.timeIntervalSinceNow > 0 && item.status != AVPlayerItemStatusFailed)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
        emit(@{@"stage": @"player_finished", @"ended": @(ended), @"status": @(item.status),
               @"seconds": @(CMTimeGetSeconds(item.currentTime)), @"error": item.error.description ?: @""});
        [player pause];
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        return ended && item.status == AVPlayerItemStatusReadyToPlay ? 0 : 1;
    }
}
