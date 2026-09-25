// Native compressed passthrough trim. No PCM conversion or codec configuration.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#include <math.h>

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
        if (argc != 5)
            return 2;
        double start = strtod(argv[3], NULL), end = strtod(argv[4], NULL);
        if (!isfinite(start) || !isfinite(end) || start < 0 || end <= start)
            return 2;
        NSString *output = [NSString stringWithUTF8String:argv[2]];
        if ([[NSFileManager defaultManager] fileExistsAtPath:output])
            return 2;
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] options:nil];
        if (end > CMTimeGetSeconds(asset.duration))
            return 2;
        AVAssetExportSession *session = [[AVAssetExportSession alloc] initWithAsset:asset
                                                                      presetName:AVAssetExportPresetPassthrough];
        emit(@{@"stage": @"export_formats", @"formats": session.supportedFileTypes ?: @[]});
        if (!session || ![session.supportedFileTypes containsObject:AVFileTypeMPEG4])
            return 1;
        session.outputURL = [NSURL fileURLWithPath:output];
        session.outputFileType = AVFileTypeMPEG4;
        session.timeRange = CMTimeRangeFromTimeToTime(CMTimeMakeWithSeconds(start, 48000),
                                                       CMTimeMakeWithSeconds(end, 48000));
        session.shouldOptimizeForNetworkUse = YES;
        __block BOOL done = NO;
        [session exportAsynchronouslyWithCompletionHandler:^{ done = YES; }];
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:25];
        while (!done && deadline.timeIntervalSinceNow > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
        if (!done)
            [session cancelExport];
        emit(@{@"stage": @"export_finished", @"status": @(session.status), @"start": @(start),
               @"end": @(end), @"seconds": @(end - start), @"error": session.error.description ?: @""});
        return done && session.status == AVAssetExportSessionStatusCompleted ? 0 : 1;
    }
}
