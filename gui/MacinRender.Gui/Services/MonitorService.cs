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

    /// <summary>导入 + 应用语义策略 + 解析后端 + 打开默认音频设备开始监听。返回 ABI 错误码。</summary>
    public AdmErrorCode Start(string inputPath, RenderSettings settings)
    {
        ArgumentException.ThrowIfNullOrEmpty(inputPath);
        ArgumentNullException.ThrowIfNull(settings);
        Stop();

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
            var rc = NativeMethods.adm_create_monitor(ctx, inputPath, opts, out var monitor);
            if (rc != AdmErrorCode.Ok || monitor.IsInvalid)
            {
                monitor.Dispose();
                ctx.Dispose();
                return rc == AdmErrorCode.Ok ? AdmErrorCode.Internal : rc;
            }

            _ctx = ctx;
            _monitor = monitor;
            return AdmErrorCode.Ok;
        }
    }

    public AdmErrorCode Play() => Call(NativeMethods.adm_monitor_play);
    public AdmErrorCode Pause() => Call(NativeMethods.adm_monitor_pause);

    public AdmErrorCode Seek(double seconds) =>
        _monitor is { IsInvalid: false } m ? NativeMethods.adm_monitor_seek(m, seconds) : AdmErrorCode.InvalidArgument;

    public AdmErrorCode SetLoop(double startSeconds, double endSeconds) =>
        _monitor is { IsInvalid: false } m
            ? NativeMethods.adm_monitor_set_loop(m, startSeconds, endSeconds)
            : AdmErrorCode.InvalidArgument;

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
        return NativeMethods.adm_monitor_switch_backend(m, opts);
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
        try
        {
            for (var i = 0; i < overrides.Count; i++)
            {
                idPtrs[i] = Marshal.StringToCoTaskMemUTF8(overrides[i].ObjectId);
                var native = new AdmMonitorOverride
                {
                    StructSize = (uint)stride,
                    ObjectId = idPtrs[i],
                    GainDb = overrides[i].GainDb,
                    DiffuseScale = overrides[i].DiffuseScale,
                    ExtentScale = overrides[i].ExtentScale,
                    DivergenceScale = overrides[i].DivergenceScale,
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

    private AdmErrorCode Call(Func<AdmMonitorHandle, AdmErrorCode> fn) =>
        _monitor is { IsInvalid: false } m ? fn(m) : AdmErrorCode.InvalidArgument;
}
