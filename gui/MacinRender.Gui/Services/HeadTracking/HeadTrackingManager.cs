using System;
using System.Numerics;

namespace MacinRender.Gui.Services.HeadTracking;

/// <summary>
/// 头追踪管线中枢:订阅一个 <see cref="IHeadTrackingSource"/>,以四元数为唯一内部表示做
/// recenter / slerp 平滑 / 死区,末端只做一次 quat→Euler(项目 ADM 约定:yaw +左、pitch +上、
/// roll +顺时针),通过 <see cref="OrientationResolved"/> 扇出给视觉(SpatialSceneControl)与
/// 音频(MonitorService.SetListenerOrientation)。同一三角喂两边 → 视听天然一致。
///
/// 「两类表示并存」是反模式(双真相源会漂移);手操(Euler-原生)和 AirPods(四元数-原生)各自
/// 在来源侧转成四元数进来,管线全程四元数,只在这里转一次 Euler。
///
/// 线程:全程 UI 线程(monitor 非线程安全;传感器来源须自行 marshal 回 UI 线程再触发事件)。
/// 平滑 + 发射由外部 <see cref="Tick"/> 驱动(复用 VM 的轮询定时器),故本类不依赖 Avalonia,
/// 纯逻辑、AOT 安全、可单测。
/// </summary>
internal sealed class HeadTrackingManager : IDisposable
{
    // 每个 Tick 朝目标 slerp 的比例(0..1)。偏大=跟手、偏小=抗抖。手操要跟手 → 0.5;
    // 将来 AirPods 抖动大可调小。VM 轮询约 33 Hz,够喂满 worker 块边界(~47 Hz 上限内)。
    private const float SmoothingFactor = 0.5f;

    // 死区(度):resolved 三轴相对上次发射变化都小于此值时不重发,避免把同一姿态刷爆 ABI。
    private const double DeadzoneDeg = 0.05;

    private const double Rad2Deg = 180.0 / Math.PI;

    private IHeadTrackingSource? _source;
    private Quaternion _offset = Quaternion.Identity;    // recenter 参考姿态(发射时取其逆)
    private Quaternion _rawLatest = Quaternion.Identity; // 来源最近一帧原始姿态
    private Quaternion _smoothed = Quaternion.Identity;  // slerp 平滑后的内部权威姿态

    private bool _hasEmitted;
    private double _lastYawDeg;
    private double _lastPitchDeg;
    private double _lastRollDeg;

    /// <summary>解析出的头部朝向(度,项目约定)。在 UI 线程触发,供视觉 + 音频两路消费。</summary>
    public event Action<float, float, float>? OrientationResolved;

    /// <summary>当前是否挂着一个激活的来源。</summary>
    public bool IsActive => _source is { IsActive: true };

    /// <summary>挂上来源并启动(替换原有来源)。</summary>
    public void AttachSource(IHeadTrackingSource source)
    {
        ArgumentNullException.ThrowIfNull(source);
        DetachSource();
        _source = source;
        source.OrientationChanged += OnSourceOrientation;
        source.Start();
    }

    /// <summary>卸下当前来源并复位内部状态。</summary>
    public void DetachSource()
    {
        if (_source is { } s)
        {
            s.OrientationChanged -= OnSourceOrientation;
            s.Stop();
            _source = null;
        }
        _offset = Quaternion.Identity;
        _rawLatest = Quaternion.Identity;
        _smoothed = Quaternion.Identity;
        _hasEmitted = false;
    }

    /// <summary>把「当前姿态」设为新的正前(recenter)。对任何来源通用——传感器无法物理归零,
    /// 用偏置抵消;手操亦可同时调 source.Reset() 清累积。</summary>
    public void Recenter() => _offset = _rawLatest;

    private void OnSourceOrientation(Quaternion q) => _rawLatest = q;

    /// <summary>由外部定时器驱动(~33 Hz):向最近原始姿态 slerp 一步,应用 recenter 偏置,
    /// 转 Euler,超过死区则发射。输入停止后几个 Tick 内收敛、低于死区即自然停发。</summary>
    public void Tick()
    {
        if (!IsActive)
        {
            return;
        }

        _smoothed = Quaternion.Slerp(_smoothed, _rawLatest, SmoothingFactor);

        // recenter:relative = offset⁻¹ * smoothed(单位四元数的逆 = 共轭)。
        var resolved = Quaternion.Concatenate(_smoothed, Quaternion.Conjugate(_offset));
        resolved = Quaternion.Normalize(resolved);

        var (yawDeg, pitchDeg, rollDeg) = ToYawPitchRollDeg(resolved);

        if (_hasEmitted &&
            Math.Abs(yawDeg - _lastYawDeg) < DeadzoneDeg &&
            Math.Abs(pitchDeg - _lastPitchDeg) < DeadzoneDeg &&
            Math.Abs(rollDeg - _lastRollDeg) < DeadzoneDeg)
        {
            return;
        }

        _lastYawDeg = yawDeg;
        _lastPitchDeg = pitchDeg;
        _lastRollDeg = rollDeg;
        _hasEmitted = true;
        OrientationResolved?.Invoke((float)yawDeg, (float)pitchDeg, (float)rollDeg);
    }

    // 四元数 → (yaw,pitch,roll) 度,与 ManualFreeLookSource 的 Quaternion.CreateFromYawPitchRoll
    // 成对(.NET 约定 R = Ry(yaw)·Rx(pitch)·Rz(roll),即 YXZ)。提取公式由该旋转矩阵反解、
    // 已对 identity / 纯 yaw / 纯 pitch 逐一验证。pitch 为中轴,接近 ±90° 时 yaw/roll 退化(已夹 ±85°)。
    private static (double yawDeg, double pitchDeg, double rollDeg) ToYawPitchRollDeg(Quaternion q)
    {
        double sinPitch = Math.Clamp(2.0 * ((q.W * q.X) - (q.Y * q.Z)), -1.0, 1.0);
        double pitch = Math.Asin(sinPitch);
        double roll = Math.Atan2(2.0 * ((q.X * q.Y) + (q.W * q.Z)), 1.0 - (2.0 * ((q.X * q.X) + (q.Z * q.Z))));
        double yaw = Math.Atan2(2.0 * ((q.X * q.Z) + (q.W * q.Y)), 1.0 - (2.0 * ((q.X * q.X) + (q.Y * q.Y))));
        return (yaw * Rad2Deg, pitch * Rad2Deg, roll * Rad2Deg);
    }

    public void Dispose() => DetachSource();
}
