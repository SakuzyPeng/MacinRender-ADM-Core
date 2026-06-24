using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Avalonia.Threading;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Services.HeadTracking;

/// <summary>
/// 真头部追踪来源:经原生 shim(libmr_headtrack → CMHeadphoneMotionManager)读 AirPods 头部姿态,
/// 产出四元数。它是 <see cref="IHeadTrackingSource"/> 的第二个实现,与 <see cref="ManualFreeLookSource"/>
/// 平级、共用下游同一条 <see cref="HeadTrackingManager"/> 四元数管线 —— 接它只是「加一个来源」,
/// 不动 manager/视觉/音频。
///
/// 线程:CoreMotion 在后台队列回调 → 经 Dispatcher.UIThread.Post marshal 回 UI 线程再触发事件
/// (monitor 非线程安全)。回调是 NativeAOT 的 [UnmanagedCallersOnly] 静态方法,经静态当前实例路由
/// (同一时刻只有一个 AirPods 来源激活)。
///
/// 注意:真正取到数据还需应用打成 .app bundle + Info.plist 的 NSMotionUsageDescription + 用户授权;
/// 裸可执行文件下 <see cref="IsAvailable"/> 仍可反映硬件在位,但 Start 取不到数据。坐标系(CoreMotion
/// 设备帧 → 项目 yaw/+左 约定)的轴重映射留待真机标定。
/// </summary>
internal sealed class AirPodsMotionSource : IHeadTrackingSource
{
    private static AirPodsMotionSource? s_current;

    public event Action<Quaternion>? OrientationChanged;

    public bool IsActive { get; private set; }

    /// <summary>硬件 + 框架是否支持(AirPods 在位)。与「权限是否已授予」无关;dylib 缺失视为不可用。</summary>
    public static bool IsAvailable()
    {
        try
        {
            return NativeMethods.mr_headtrack_available() != 0;
        }
        catch (DllNotFoundException)
        {
            return false; // shim 未构建(build-headtrack.sh 未跑)
        }
        catch (EntryPointNotFoundException)
        {
            return false;
        }
    }

    public unsafe void Start()
    {
        if (IsActive)
        {
            return;
        }
        s_current = this;
        if (NativeMethods.mr_headtrack_start(&OnNativeSample) != 0)
        {
            IsActive = true;
        }
        else
        {
            s_current = null; // 不可用 / 未授权
        }
    }

    public void Stop()
    {
        if (!IsActive)
        {
            return;
        }
        NativeMethods.mr_headtrack_stop();
        IsActive = false;
        if (ReferenceEquals(s_current, this))
        {
            s_current = null;
        }
    }

    // CoreMotion 后台队列回调(AOT 静态蹦床)→ 路由到当前实例 → marshal 回 UI 线程触发事件。
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnNativeSample(double w, double x, double y, double z)
    {
        var src = s_current;
        if (src is null)
        {
            return;
        }
        // System.Numerics.Quaternion 构造为 (x, y, z, w);CMQuaternion 给的是 (w, x, y, z)。
        var q = new Quaternion((float)x, (float)y, (float)z, (float)w);
        Dispatcher.UIThread.Post(() => src.OrientationChanged?.Invoke(q));
    }

    public void Dispose() => Stop();
}
