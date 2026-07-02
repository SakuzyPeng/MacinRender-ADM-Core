using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Services;

/// <summary>
/// 实时监听服务:经 P/Invoke 调 adm_monitor_*(C ABI v1.15–v1.17)。
///
/// 线程/生命周期遵循 c_api.h 契约:
///  - 单个 monitor 非线程安全 → 所有调用必须串行(GUI 统一在 UI 线程调用)。
///  - 状态/电平/日志均轮询(无回调进托管代码),由 UI 定时器拉取(§11)。
///  - SafeHandle 引用计数保证句柄在 P/Invoke 期间不被释放;Stop/Dispose 先放 monitor 再放 context。
/// </summary>
public sealed class MonitorService : IDisposable
{
    private AdmContextHandle? _ctx;
    private AdmMonitorHandle? _monitor;

    public bool IsActive => _monitor is { IsInvalid: false };
    public string? LastStartFailureDetails { get; private set; }
    public string? LastOperationFailureDetails { get; private set; }

    /// <summary>枚举系统输出设备(adm_monitor_output_devices_json)。失败 / 无设备返回空列表。
    /// 不需要活动会话:内部开一个临时 context 查询。</summary>
    public static IReadOnlyList<MonitorDevice> ListOutputDevices()
    {
        using var ctx = NativeMethods.adm_create_context();
        if (ctx.IsInvalid)
        {
            return System.Array.Empty<MonitorDevice>();
        }

        var dtos = AdmQueries.LoadOutputDevices(ctx);
        if (dtos is null)
        {
            return System.Array.Empty<MonitorDevice>();
        }

        var list = new List<MonitorDevice>(dtos.Count);
        foreach (var d in dtos)
        {
            list.Add(new MonitorDevice(d.Id, d.Name, d.Default));
        }

        return list;
    }

    /// <summary>导入 + 应用语义策略 + 解析后端 + 打开输出设备开始监听。deviceId 为 null/"" = 默认设备。
    /// 返回 ABI 错误码。</summary>
    public AdmErrorCode Start(string inputPath, RenderSettings settings, string? deviceId = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(inputPath);
        ArgumentNullException.ThrowIfNull(settings);
        Stop();
        LastStartFailureDetails = null;
        LastOperationFailureDetails = null;

        var ctx = NativeMethods.adm_create_context();
        if (ctx.IsInvalid)
        {
            ctx.Dispose();
            return AdmErrorCode.Internal;
        }

        using (var opts = NativeMethods.adm_create_render_options())
        {
            if (opts.IsInvalid)
            {
                ctx.Dispose();
                return AdmErrorCode.Internal;
            }

            AdmRenderService.ApplySettings(opts, settings);
            var rc = NativeMethods.adm_create_monitor_ex(ctx, inputPath, opts, deviceId, out var monitor);
            if (rc != AdmErrorCode.Ok || monitor.IsInvalid)
            {
                LastStartFailureDetails = ReadLastError(ctx);
                monitor.Dispose();
                ctx.Dispose();
                return rc == AdmErrorCode.Ok ? AdmErrorCode.Internal : rc;
            }

            _ctx = ctx;
            _monitor = monitor;
            return AdmErrorCode.Ok;
        }
    }

    private static string? ReadLastError(AdmContextHandle ctx)
    {
        var ptr = NativeMethods.adm_context_last_error_message(ctx);
        var text = ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
        return string.IsNullOrWhiteSpace(text) ? null : text;
    }

    public AdmErrorCode Play() => Call(NativeMethods.adm_monitor_play);
    public AdmErrorCode Pause() => Call(NativeMethods.adm_monitor_pause);

    public AdmErrorCode Seek(double seconds)
    {
        LastOperationFailureDetails = null;
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        var rc = NativeMethods.adm_monitor_seek(m, seconds);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    public AdmErrorCode SetLoop(double startSeconds, double endSeconds)
    {
        LastOperationFailureDetails = null;
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        var rc = NativeMethods.adm_monitor_set_loop(m, startSeconds, endSeconds);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    /// <summary>实时设置听者头部朝向(yaw/pitch/roll 度;yaw +左)。Apple 与 SAF 双耳监听后端实装,其它后端忽略。
    /// 契约:monitor 非线程安全,须在 UI 线程调用。</summary>
    public AdmErrorCode SetListenerOrientation(float yawDeg, float pitchDeg, float rollDeg)
    {
        LastOperationFailureDetails = null;
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        var rc = NativeMethods.adm_monitor_set_listener_orientation(m, yawDeg, pitchDeg, rollDeg);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    /// <summary>播放中即时切换输出设备(deviceId 为 null/"" = 默认设备)。保留播放头/状态/后端/覆盖。</summary>
    public AdmErrorCode SetOutputDevice(string? deviceId)
    {
        LastOperationFailureDetails = null;
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        var rc = NativeMethods.adm_monitor_set_output_device(m, deviceId);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    /// <summary>热切换后端 / 布局(立体声监听下可跨布局,经下混)。</summary>
    public AdmErrorCode SwitchBackend(RenderSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        using var opts = NativeMethods.adm_create_render_options();
        if (opts.IsInvalid)
        {
            return AdmErrorCode.Internal;
        }

        AdmRenderService.ApplySettings(opts, settings);
        LastOperationFailureDetails = null;
        var rc = NativeMethods.adm_monitor_switch_backend(m, opts);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    /// <summary>替换全部按对象覆盖(列表为空表示清空)。revision 经 status.OverrideRevision 回显。</summary>
    public AdmErrorCode SetOverrides(IReadOnlyList<MonitorOverride> overrides, ulong revision)
    {
        ArgumentNullException.ThrowIfNull(overrides);
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }
        if (overrides.Count == 0)
        {
            return NativeMethods.adm_monitor_set_overrides(m, IntPtr.Zero, 0, revision);
        }

        var stride = Marshal.SizeOf<AdmMonitorOverride>();
        var arr = Marshal.AllocHGlobal(stride * overrides.Count);
        var idPtrs = new IntPtr[overrides.Count];
        var labelPtrs = new IntPtr[overrides.Count];
        try
        {
            for (var i = 0; i < overrides.Count; i++)
            {
                idPtrs[i] = Marshal.StringToCoTaskMemUTF8(overrides[i].ObjectId);
                labelPtrs[i] = string.IsNullOrEmpty(overrides[i].SpeakerLabel)
                    ? IntPtr.Zero
                    : Marshal.StringToCoTaskMemUTF8(overrides[i].SpeakerLabel);
                var native = new AdmMonitorOverride
                {
                    StructSize = (uint)stride,
                    ObjectId = idPtrs[i],
                    GainDb = overrides[i].GainDb,
                    DiffuseScale = overrides[i].DiffuseScale,
                    ExtentScale = overrides[i].ExtentScale,
                    DivergenceScale = overrides[i].DivergenceScale,
                    ExtentWidthScale = overrides[i].ExtentWidthScale,
                    ExtentHeightScale = overrides[i].ExtentHeightScale,
                    ExtentDepthScale = overrides[i].ExtentDepthScale,
                    SpeakerLabel = labelPtrs[i],
                    HeadLocked = overrides[i].HeadLocked ? 1 : 0,
                };
                Marshal.StructureToPtr(native, arr + (i * stride), false);
            }

            return NativeMethods.adm_monitor_set_overrides(m, arr, (uint)overrides.Count, revision);
        }
        finally
        {
            foreach (var p in idPtrs)
            {
                if (p != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(p);
                }
            }

            foreach (var p in labelPtrs)
            {
                if (p != IntPtr.Zero)
                {
                    Marshal.FreeCoTaskMem(p);
                }
            }

            Marshal.FreeHGlobal(arr);
        }
    }

    public MonitorStatusSnapshot? GetStatus()
    {
        if (_monitor is not { IsInvalid: false } m)
        {
            return null;
        }

        var st = new AdmMonitorStatus { StructSize = (uint)Marshal.SizeOf<AdmMonitorStatus>() };
        if (NativeMethods.adm_monitor_get_status(m, ref st) != AdmErrorCode.Ok)
        {
            return null;
        }

        return new MonitorStatusSnapshot(
            (AdmMonitorState)st.State, st.PlayheadFrames, st.Underruns, st.BufferedFrames, st.RingFill,
            st.Ended != 0, st.Failed != 0, st.OverrideRevision);
    }

    public unsafe MonitorLevelsSnapshot? GetLevels(int capacity)
    {
        if (_monitor is not { IsInvalid: false } m || capacity <= 0)
        {
            return null;
        }

        var peak = new float[capacity];
        var rms = new float[capacity];
        fixed (float* pPeak = peak, pRms = rms)
        {
            var levels = new AdmMonitorLevels
            {
                StructSize = (uint)Marshal.SizeOf<AdmMonitorLevels>(),
                Capacity = (uint)capacity,
                Peak = (IntPtr)pPeak,
                Rms = (IntPtr)pRms,
            };
            if (NativeMethods.adm_monitor_get_levels(m, ref levels) != AdmErrorCode.Ok)
            {
                return null;
            }

            var n = (int)Math.Min(levels.OutCount, (uint)capacity);
            return new MonitorLevelsSnapshot(
                peak[..n], rms[..n], levels.MomentaryLufs, levels.ShorttermLufs, levels.IntegratedLufs);
        }
    }

    /// <summary>读取诊断日志条目 [fromIndex, count)。返回当前总条数,便于增量轮询。</summary>
    public uint ReadLogs(uint fromIndex, List<RenderLogEntry> into)
    {
        ArgumentNullException.ThrowIfNull(into);
        if (_monitor is not { IsInvalid: false } m)
        {
            return 0;
        }

        var count = NativeMethods.adm_monitor_log_count(m);
        for (var i = fromIndex; i < count; i++)
        {
            if (NativeMethods.adm_monitor_log_entry(m, i, out var level, out var modulePtr, out var messagePtr) == 1)
            {
                var module = Marshal.PtrToStringUTF8(modulePtr) ?? string.Empty;
                var message = Marshal.PtrToStringUTF8(messagePtr) ?? string.Empty;
                into.Add(new RenderLogEntry(level, module, message));
            }
        }

        return count;
    }

    public void Stop()
    {
        _monitor?.Dispose(); // stops device + joins worker first (borrows nothing from ctx after create)
        _monitor = null;
        _ctx?.Dispose();
        _ctx = null;
    }

    public void Dispose() => Stop();

    private AdmErrorCode Call(Func<AdmMonitorHandle, AdmErrorCode> fn)
    {
        LastOperationFailureDetails = null;
        if (_monitor is not { IsInvalid: false } m)
        {
            return AdmErrorCode.InvalidArgument;
        }

        var rc = fn(m);
        if (rc != AdmErrorCode.Ok)
        {
            LastOperationFailureDetails = ReadLastError(m);
        }
        return rc;
    }

    private static string? ReadLastError(AdmMonitorHandle monitor)
    {
        var ptr = NativeMethods.adm_monitor_last_error_message(monitor);
        var text = ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
        return string.IsNullOrWhiteSpace(text) ? null : text;
    }
}
