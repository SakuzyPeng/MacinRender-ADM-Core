using System;
using System.Numerics;

namespace MacinRender.Gui.Services.HeadTracking;

/// <summary>
/// 头部追踪「来源」抽象——镜像渲染后端的 IRenderer 思路:每个来源只负责产出一串
/// 头部朝向采样,完全不关心下游怎么用。手操自由视角(<see cref="ManualFreeLookSource"/>)与
/// 将来的 AirPods/CoreMotion、OpenTrack、VR 等都是这个接口背后的平级实现。
///
/// 约定:
///  - 朝向以 <see cref="Quaternion"/>(无万向锁、无损超集)对外表达;Euler-原生的来源
///    在自己内部累积后转成四元数发出,四元数-原生的来源(AirPods)零转换直接发。
///  - <see cref="OrientationChanged"/> 在 UI 线程触发(monitor 非线程安全;传感器来源须
///    自行 Dispatcher.Post 回 UI 线程)。下游 HeadTrackingManager 负责 recenter/平滑/节流。
/// </summary>
internal interface IHeadTrackingSource : IDisposable
{
    /// <summary>产出一个新的「绝对」头部朝向(相对来源自身原点;recenter 由 manager 负责)。
    /// 必须在 UI 线程触发。</summary>
    event Action<Quaternion>? OrientationChanged;

    /// <summary>来源是否正在产出采样。</summary>
    bool IsActive { get; }

    /// <summary>开始产出(手操来源是空操作 + 置位;传感器来源在此启动设备/回调)。</summary>
    void Start();

    /// <summary>停止产出并释放底层资源(传感器、回调)。可重复调用。</summary>
    void Stop();
}
