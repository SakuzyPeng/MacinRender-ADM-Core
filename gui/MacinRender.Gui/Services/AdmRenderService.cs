using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Services;

/// <summary>
/// 真实渲染服务:经 P/Invoke 调 adm_render_file_ex2(结构化进度 + 协作式取消)。
///
/// 线程/生命周期遵循 c_api.h 契约:
///  - 单个 context 非线程安全 → 每次渲染用独立 context(本服务无共享状态,可并发实例)。
///  - cancel token 可跨线程 → 用 CancellationToken.Register 把托管取消桥到 adm_cancel,
///    回调在 worker 线程渲染期间被 UI 线程触发是安全的。
///  - SafeHandle 引用计数保证句柄在 P/Invoke 调用期间不被释放;ctReg 在 token 之前 Dispose,
///    CancellationTokenRegistration.Dispose 会同步等待进行中的 adm_cancel 回调返回。
/// </summary>
public sealed class AdmRenderService
{
    public Task<RenderOutcome> RenderAsync(string inputPath, string? outputPath, RenderSettings settings,
        IProgress<RenderProgress>? progress, CancellationToken ct)
    {
        ArgumentException.ThrowIfNullOrEmpty(inputPath);
        ArgumentNullException.ThrowIfNull(settings);
        return Task.Run(() => RenderBlocking(inputPath, outputPath, settings, progress, ct), ct);
    }

    private static unsafe RenderOutcome RenderBlocking(string inputPath, string? outputPath, RenderSettings s,
        IProgress<RenderProgress>? progress, CancellationToken ct)
    {
        using var ctx = NativeMethods.adm_create_context();
        if (ctx.IsInvalid)
        {
            return Fail(AdmErrorCode.Internal, "无法创建 ADM context");
        }

        using var opts = NativeMethods.adm_create_render_options();
        if (opts.IsInvalid)
        {
            return Fail(AdmErrorCode.Internal, "无法创建 render options");
        }

        ApplySettings(opts, s);

        using var token = NativeMethods.adm_create_cancel_token();
        if (!token.IsInvalid)
        {
            NativeMethods.adm_render_options_set_cancel_token(opts, token);
        }

        GCHandle gch = default;
        IntPtr progressPtr = IntPtr.Zero;
        IntPtr userData = IntPtr.Zero;
        if (progress is not null)
        {
            gch = GCHandle.Alloc(progress);
            userData = GCHandle.ToIntPtr(gch);
            delegate* unmanaged[Cdecl]<AdmProgressEventV2*, IntPtr, void> fn = &ProgressTrampoline;
            progressPtr = (IntPtr)fn;
        }

        try
        {
            // 最后注册 → 最先 Dispose:确保停止取消回调(并等其返回)后,token 才被释放。
            using var ctReg = ct.Register(static state =>
                NativeMethods.adm_cancel((AdmCancelTokenHandle)state!), token);

            var rc = NativeMethods.adm_render_file_ex2(ctx, inputPath, outputPath, opts,
                progressPtr, userData, out var result);

            using (result)
            {
                return BuildOutcome(rc, result);
            }
        }
        finally
        {
            if (gch.IsAllocated)
            {
                gch.Free();
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe void ProgressTrampoline(AdmProgressEventV2* evt, IntPtr userData)
    {
        if (evt is null || userData == IntPtr.Zero)
        {
            return;
        }

        if (GCHandle.FromIntPtr(userData).Target is not IProgress<RenderProgress> progress)
        {
            return;
        }

        string? message = evt->Message != IntPtr.Zero ? Marshal.PtrToStringUTF8(evt->Message) : null;
        progress.Report(new RenderProgress(evt->OverallFraction, evt->Stage, evt->Operation, message));
    }

    private static void ApplySettings(AdmRenderOptionsHandle opts, RenderSettings s)
    {
        NativeMethods.adm_render_options_set_renderer(opts, s.Renderer);
        NativeMethods.adm_render_options_set_output_layout(opts, s.Layout);

        if (s.BitDepth is { } depth)
        {
            NativeMethods.adm_render_options_set_output_bit_depth(opts, depth);
        }

        if (s.OpusBitratePerChKbps is { } opus)
        {
            NativeMethods.adm_render_options_set_opus_bitrate_per_ch_kbps(opts, opus);
        }

        if (s.ApacBitrateKbps is { } apac)
        {
            NativeMethods.adm_render_options_set_apac_bitrate_kbps(opts, apac);
        }

        if (s.ApacContainer is { } container)
        {
            NativeMethods.adm_render_options_set_apac_container(opts, container);
        }

        if (s.LoudnessTargetLufs is { } lufs)
        {
            NativeMethods.adm_render_options_set_loudness_target(opts, lufs);
        }

        if (!string.IsNullOrEmpty(s.SofaPath))
        {
            NativeMethods.adm_render_options_set_sofa_path(opts, s.SofaPath);
        }

        if (!string.IsNullOrEmpty(s.SemanticPolicyJson))
        {
            NativeMethods.adm_render_options_set_semantic_policy_json(opts, s.SemanticPolicyJson);
        }
    }

    private static RenderOutcome BuildOutcome(AdmErrorCode rc, AdmRenderResultHandle result)
    {
        if (result.IsInvalid)
        {
            return Fail(rc, rc == AdmErrorCode.Ok ? "渲染完成(无结果句柄)" : $"渲染失败:{rc}");
        }

        var errorCode = NativeMethods.adm_render_result_error_code(result);
        string message = PtrToString(NativeMethods.adm_render_result_message(result)) ?? string.Empty;
        string? outputPath = PtrToString(NativeMethods.adm_render_result_output_path(result));

        double? loudness = NativeMethods.adm_render_result_loudness_lufs(result, out var l) == 1 ? l : null;
        double? peak = NativeMethods.adm_render_result_peak_dbtp(result, out var p) == 1 ? p : null;

        var logs = ReadLogs(result);
        bool success = errorCode == AdmErrorCode.Ok;
        return new RenderOutcome(success, errorCode, message, outputPath, loudness, peak, logs);
    }

    private static IReadOnlyList<RenderLogEntry> ReadLogs(AdmRenderResultHandle result)
    {
        uint count = NativeMethods.adm_render_result_log_count(result);
        if (count == 0)
        {
            return Array.Empty<RenderLogEntry>();
        }

        var list = new List<RenderLogEntry>((int)count);
        for (uint i = 0; i < count; i++)
        {
            if (NativeMethods.adm_render_result_log_entry(result, i, out var level, out var modulePtr,
                    out var messagePtr) == 1)
            {
                list.Add(new RenderLogEntry(level, PtrToString(modulePtr) ?? string.Empty,
                    PtrToString(messagePtr) ?? string.Empty));
            }
        }

        return list;
    }

    private static RenderOutcome Fail(AdmErrorCode code, string message) =>
        new(false, code, message, null, null, null, Array.Empty<RenderLogEntry>());

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static string? PtrToString(IntPtr p) => p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p);
}
