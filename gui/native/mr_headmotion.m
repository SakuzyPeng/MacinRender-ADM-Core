#include <math.h>

#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

#include "mr_headmotion.h"

@interface MRHeadMotionSession : NSObject
@property(nonatomic, strong) CMHeadphoneMotionManager* manager;
@property(nonatomic, strong) NSOperationQueue* delivery;
@property(nonatomic, strong) NSLock* sampleLock;
@property(nonatomic, strong) CMDeviceMotion* latest;
@property(nonatomic) BOOL deliveryError;
@property(nonatomic) uint64_t sequence;
@property(nonatomic) uint64_t observedSequence;
@property(nonatomic) double receivedAt;
@property(nonatomic) double attemptedAt;
@property(nonatomic) BOOL requested;
- (mr_headmotion_sample_t)sample;
- (void)stop;
@end

@implementation MRHeadMotionSession
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _delivery = [[NSOperationQueue alloc] init];
        _delivery.maxConcurrentOperationCount = 1;
        _sampleLock = [[NSLock alloc] init];
    }
    return self;
}
- (mr_headmotion_sample_t)sample {
    mr_headmotion_sample_t result = {0};
    result.struct_size = sizeof(result);
    result.w = 1.0;
    if ([[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMotionUsageDescription"] == nil) {
        result.state = 5;
        return result;
    }
    if (@available(macOS 14.0, *)) {
        const CMAuthorizationStatus authorization = [CMHeadphoneMotionManager authorizationStatus];
        if (authorization == CMAuthorizationStatusDenied || authorization == CMAuthorizationStatusRestricted) {
            [self stop];
            result.state = 3;
            return result;
        }
        if (self.manager == nil) {
            self.manager = [[CMHeadphoneMotionManager alloc] init];
        }
        if (!self.manager.isDeviceMotionAvailable) {
            [self stop];
            result.state = 4;
            return result;
        }
        const double now = NSProcessInfo.processInfo.systemUptime;
        if (!self.requested) {
            if (self.attemptedAt > 0.0 && now - self.attemptedAt < 2.0) {
                result.state = 4;
                return result;
            }
            self.attemptedAt = now;
            self.requested = YES;
            __weak MRHeadMotionSession* weakSelf = self;
            [self.manager startDeviceMotionUpdatesToQueue:self.delivery
                                              withHandler:^(CMDeviceMotion* motion, NSError* error) {
                                                MRHeadMotionSession* session = weakSelf;
                                                if (session == nil) {
                                                    return;
                                                }
                                                [session.sampleLock lock];
                                                session.latest = motion;
                                                session.deliveryError = error != nil;
                                                if (motion != nil) {
                                                    session.sequence += 1;
                                                }
                                                [session.sampleLock unlock];
                                              }];
        }
        [self.sampleLock lock];
        CMDeviceMotion* motion = self.latest;
        const BOOL error = self.deliveryError;
        const uint64_t sequence = self.sequence;
        [self.sampleLock unlock];
        if (error) {
            [self stop];
            result.state = 4;
            return result;
        }
        if (motion == nil) {
            result.state = 1;
            return result;
        }
        // Sensor timestamps are not compared with the host's uptime. Freshness
        // follows deliveries, measured entirely in the host clock domain.
        if (sequence != self.observedSequence) {
            self.observedSequence = sequence;
            self.receivedAt = now;
        }
        const CMQuaternion q = motion.attitude.quaternion;
        if (now - self.receivedAt > 2.0 || !isfinite(q.w) || !isfinite(q.x) || !isfinite(q.y) || !isfinite(q.z)) {
            [self stop];
            result.state = 4;
            return result;
        }
        result.state = 2;
        result.sequence = sequence;
        result.timestamp = motion.timestamp;
        result.w = q.w;
        result.x = q.x;
        result.y = q.y;
        result.z = q.z;
    }
    return result;
}
- (void)stop {
    [self.manager stopDeviceMotionUpdates];
    [self.delivery cancelAllOperations];
    if (NSOperationQueue.currentQueue != self.delivery) {
        [self.delivery waitUntilAllOperationsAreFinished];
    }
    [self.sampleLock lock];
    self.latest = nil;
    self.deliveryError = NO;
    [self.sampleLock unlock];
    self.requested = NO;
    self.receivedAt = 0.0;
}
- (void)dealloc {
    [self stop];
}
@end

mr_headmotion_t* mr_headmotion_create(void) {
    @autoreleasepool {
        return (__bridge_retained mr_headmotion_t*) [[MRHeadMotionSession alloc] init];
    }
}

int mr_headmotion_poll(mr_headmotion_t* handle, mr_headmotion_sample_t* out) {
    if (handle == NULL || out == NULL || out->struct_size < sizeof(*out)) {
        return 0;
    }
    @autoreleasepool {
        MRHeadMotionSession* session = (__bridge MRHeadMotionSession*) handle;
        // Service Foundation input sources on the independent control thread.
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate date]];
        *out = [session sample];
        return 1;
    }
}

void mr_headmotion_destroy(mr_headmotion_t* handle) {
    @autoreleasepool {
        MRHeadMotionSession* session = (__bridge_transfer MRHeadMotionSession*) handle;
        [session stop];
    }
}
