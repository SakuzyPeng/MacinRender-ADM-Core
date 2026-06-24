using System;
using System.Numerics;

namespace MacinRender.Gui.Services.HeadTracking;

/// <summary>
/// 手操自由视角来源:无硬件,由 UI(鼠标拖拽 / 触控板双指 / 键盘)喂增量。它是
/// <see cref="IHeadTrackingSource"/> 的第一个实现,用来零硬件验证整条头追踪管线;Phase 2 的
/// AirPods 来源是它的平级兄弟,共用下游同一条四元数管线。
///
/// 内部以 yaw/pitch/roll(度)累积——这正是鼠标两轴 + roll 键的「原生」输入空间,等同于
/// AirPods 来源的「原生」四元数空间。在发出边界转成四元数(其 native→canonical 那一跳,无损)。
/// pitch 夹在 ±k_pitch_limit 防翻转;yaw/roll 自由累积(wrap 由四元数天然处理)。
/// </summary>
internal sealed class ManualFreeLookSource : IHeadTrackingSource
{
    // 抬头/低头夹角上限(度):略小于 90° 避免第一人称相机在天顶/地底翻转、也避免 yaw/roll 退化。
    private const double PitchLimitDeg = 85.0;

    private double _yawDeg;
    private double _pitchDeg;
    private double _rollDeg;

    public event Action<Quaternion>? OrientationChanged;

    public bool IsActive { get; private set; }

    public void Start() => IsActive = true;

    public void Stop() => IsActive = false;

    /// <summary>累积一次「环顾」增量(度):dYaw 水平转头(+左),dPitch 俯仰(+上)。pitch 被夹住。</summary>
    public void ApplyLookDelta(double dYawDeg, double dPitchDeg)
    {
        if (!IsActive)
        {
            return;
        }
        _yawDeg += dYawDeg;
        _pitchDeg = Math.Clamp(_pitchDeg + dPitchDeg, -PitchLimitDeg, PitchLimitDeg);
        Emit();
    }

    /// <summary>累积一次歪头(roll)增量(度,+顺时针)。仅键盘 Q/E 驱动。</summary>
    public void ApplyRollDelta(double dRollDeg)
    {
        if (!IsActive)
        {
            return;
        }
        _rollDeg += dRollDeg;
        Emit();
    }

    /// <summary>把朝向归零(recenter 的来源侧:把当前手操姿态清回正前)。</summary>
    public void Reset()
    {
        _yawDeg = 0.0;
        _pitchDeg = 0.0;
        _rollDeg = 0.0;
        if (IsActive)
        {
            Emit();
        }
    }

    private void Emit()
    {
        // 度→弧度,构成四元数。约定与 manager 的 sink 转换成对(yaw=+左,pitch=+上,roll=+顺时针);
        // 具体到 AUSpatialMixer 的符号镜像由 core 的 apple 后端处理,这里只保证 source↔manager 一致。
        const double deg2rad = Math.PI / 180.0;
        var q = Quaternion.CreateFromYawPitchRoll(
            (float)(_yawDeg * deg2rad),
            (float)(_pitchDeg * deg2rad),
            (float)(_rollDeg * deg2rad));
        OrientationChanged?.Invoke(q);
    }

    public void Dispose() => Stop();
}
