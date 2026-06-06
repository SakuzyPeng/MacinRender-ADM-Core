using System;
using System.Runtime.InteropServices;

namespace MacinRender.Gui.Interop;

/// <summary>
/// P/Invoke 绑定:每个声明逐一对应 include/adm/c_api.h(stable v1.11)的导出函数。
/// 用 LibraryImport 源生成器(NativeAOT 友好,无运行时反射 marshalling stub)。
/// 字符串统一 UTF-8;owned char* 返回值用 IntPtr,调用方决定是否 adm_free_string。
/// 进度回调/userData 用 IntPtr(函数指针 + GCHandle),具体类型在服务层处理。
/// </summary>
internal static partial class NativeMethods
{
    private const string Lib = "mradm_capi";

    // ── 版本 ──
    [LibraryImport(Lib)]
    internal static partial int adm_api_version_major();

    [LibraryImport(Lib)]
    internal static partial int adm_api_version_minor();

    [LibraryImport(Lib)]
    internal static partial int adm_api_version_patch();

    // ── context ──
    [LibraryImport(Lib)]
    internal static partial AdmContextHandle adm_create_context();

    [LibraryImport(Lib)]
    internal static partial void adm_destroy_context(IntPtr context);

    // ── options:生命周期 ──
    [LibraryImport(Lib)]
    internal static partial AdmRenderOptionsHandle adm_create_render_options();

    [LibraryImport(Lib)]
    internal static partial void adm_destroy_render_options(IntPtr opts);

    // ── options:setter ──
    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_renderer(AdmRenderOptionsHandle opts,
        AdmRenderer renderer);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial AdmErrorCode adm_render_options_set_output_layout(AdmRenderOptionsHandle opts,
        string layout);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_output_bit_depth(AdmRenderOptionsHandle opts,
        AdmOutputBitDepth depth);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_loudness_target(AdmRenderOptionsHandle opts,
        double lufs);

    [LibraryImport(Lib)]
    internal static partial void adm_render_options_set_peak_limit(AdmRenderOptionsHandle opts, int enabled);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_peak_limit_dbtp(AdmRenderOptionsHandle opts,
        double dbtp);

    [LibraryImport(Lib)]
    internal static partial void adm_render_options_set_peak_normalize_to_limit(AdmRenderOptionsHandle opts,
        int enabled);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_opus_bitrate_per_ch_kbps(AdmRenderOptionsHandle opts,
        uint kbps);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_apac_bitrate_kbps(AdmRenderOptionsHandle opts,
        uint kbps);

    [LibraryImport(Lib)]
    internal static partial void adm_render_options_set_apac_drc_music(AdmRenderOptionsHandle opts, int enabled);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_apac_container(AdmRenderOptionsHandle opts,
        AdmApacContainer container);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial AdmErrorCode adm_render_options_set_sofa_path(AdmRenderOptionsHandle opts,
        string? sofaPath);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial AdmErrorCode adm_render_options_set_semantic_policy_json(AdmRenderOptionsHandle opts,
        string? json);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_speaker_spread_mode(AdmRenderOptionsHandle opts,
        AdmSpeakerSpreadMode mode);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_options_set_binaural_spread_mode(AdmRenderOptionsHandle opts,
        AdmBinauralSpreadMode mode);

    [LibraryImport(Lib)]
    internal static partial void adm_render_options_set_cancel_token(AdmRenderOptionsHandle opts,
        AdmCancelTokenHandle token);

    // ── 取消(token 跨线程安全) ──
    [LibraryImport(Lib)]
    internal static partial AdmCancelTokenHandle adm_create_cancel_token();

    [LibraryImport(Lib)]
    internal static partial void adm_destroy_cancel_token(IntPtr token);

    [LibraryImport(Lib)]
    internal static partial void adm_cancel(AdmCancelTokenHandle token);

    [LibraryImport(Lib)]
    internal static partial void adm_reset_cancel_token(AdmCancelTokenHandle token);

    // ── 渲染(结构化进度 v2) ──
    // progress = delegate* unmanaged[Cdecl]<AdmProgressEventV2*, IntPtr, void> 的地址;NULL 表示不要进度。
    // userData = GCHandle.ToIntPtr。output 可为 null(自动派生路径)。
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial AdmErrorCode adm_render_file_ex2(AdmContextHandle context, string inputPath,
        string? outputPath, AdmRenderOptionsHandle opts, IntPtr progress, IntPtr userData,
        out AdmRenderResultHandle result);

    // ── result ──
    [LibraryImport(Lib)]
    internal static partial void adm_destroy_render_result(IntPtr result);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_render_result_error_code(AdmRenderResultHandle result);

    // 返回值 const char*,由 result 拥有,勿 free。
    [LibraryImport(Lib)]
    internal static partial IntPtr adm_render_result_message(AdmRenderResultHandle result);

    [LibraryImport(Lib)]
    internal static partial IntPtr adm_render_result_output_path(AdmRenderResultHandle result);

    // 返回 1 表示有值并写入 out;0 表示该指标缺失。
    [LibraryImport(Lib)]
    internal static partial int adm_render_result_loudness_lufs(AdmRenderResultHandle result, out double value);

    [LibraryImport(Lib)]
    internal static partial int adm_render_result_peak_dbtp(AdmRenderResultHandle result, out double value);

    [LibraryImport(Lib)]
    internal static partial uint adm_render_result_log_count(AdmRenderResultHandle result);

    // 返回 1 表示 index 有效并写入各 out;module/message 是 result 拥有的 const char*,勿 free。
    [LibraryImport(Lib)]
    internal static partial int adm_render_result_log_entry(AdmRenderResultHandle result, uint index,
        out AdmLogLevel level, out IntPtr module, out IntPtr message);

    // ── owned string 释放 ──
    [LibraryImport(Lib)]
    internal static partial void adm_free_string(IntPtr s);

    // ── 查询(JSON-out,out 字符串由调用方 adm_free_string) ──
    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_output_formats_json(AdmContextHandle context, out IntPtr outJson);

    [LibraryImport(Lib)]
    internal static partial AdmErrorCode adm_capabilities_json(AdmContextHandle context, out IntPtr outJson);

    // ── probe / scene info ──
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial AdmErrorCode adm_probe_file(AdmContextHandle context, string inputPath,
        out AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial void adm_destroy_scene_info(IntPtr info);

    [LibraryImport(Lib)]
    internal static partial uint adm_scene_info_sample_rate(AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial uint adm_scene_info_channels(AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial ulong adm_scene_info_frames(AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial double adm_scene_info_duration_seconds(AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial uint adm_scene_info_programme_count(AdmSceneInfoHandle info);

    [LibraryImport(Lib)]
    internal static partial uint adm_scene_info_object_count(AdmSceneInfoHandle info);
}
