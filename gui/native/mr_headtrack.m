// MacinRender 头部追踪原生 shim(macOS-only)。包一层 CMHeadphoneMotionManager(AirPods 等
// 头部姿态),把每帧的姿态四元数(w,x,y,z)经 C 回调抛给 GUI(C# AirPodsMotionSource)。
//
// 架构边界:CoreMotion 不是音频框架,绝不进 MIT core(ADR 0003)。它只活在 GUI 这一个原生文件里,
// 非 macOS 不编译;C# 侧经 P/Invoke 调用。回调在 CoreMotion 的后台队列触发,C# 侧负责 marshal 回
// UI 线程(monitor 非线程安全)。
//
// 注意:真正拿到数据还需应用打成 .app bundle + Info.plist 的 NSMotionUsageDescription + 用户授权;
// 裸可执行文件下 isDeviceMotionAvailable 仍可反映硬件在位,但 startDeviceMotionUpdates 取不到数据。
//
// 构建:gui/build-headtrack.sh → runtimes/osx-arm64/native/libmr_headtrack.dylib(不入 git)。

#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>

typedef void (*mr_headtrack_cb)(double w, double x, double y, double z);

static CMHeadphoneMotionManager *g_manager = nil;
static mr_headtrack_cb g_callback = NULL;

static CMHeadphoneMotionManager *mr_manager(void) {
    if (g_manager == nil) {
        g_manager = [[CMHeadphoneMotionManager alloc] init];
    }
    return g_manager;
}

// 硬件 + 框架是否支持头部姿态(AirPods 在位)。与「权限是否已授予」无关。
int mr_headtrack_available(void) {
    return mr_manager().isDeviceMotionAvailable ? 1 : 0;
}

// 开始推送姿态;cb 在后台队列被反复调用,传姿态四元数(w,x,y,z)。返回 1 成功、0 不可用。
int mr_headtrack_start(mr_headtrack_cb cb) {
    CMHeadphoneMotionManager *m = mr_manager();
    if (!m.isDeviceMotionAvailable) {
        return 0;
    }
    g_callback = cb;
    NSOperationQueue *queue = [[NSOperationQueue alloc] init];
    [m startDeviceMotionUpdatesToQueue:queue
                           withHandler:^(CMDeviceMotion *_Nullable motion, NSError *_Nullable error) {
                               if (motion == nil || g_callback == NULL) {
                                   return;
                               }
                               CMQuaternion q = motion.attitude.quaternion;
                               g_callback(q.w, q.x, q.y, q.z);
                           }];
    return 1;
}

void mr_headtrack_stop(void) {
    if (g_manager != nil) {
        [g_manager stopDeviceMotionUpdates];
    }
    g_callback = NULL;
}
