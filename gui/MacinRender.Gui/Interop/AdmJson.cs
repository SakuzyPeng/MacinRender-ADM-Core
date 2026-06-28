using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace MacinRender.Gui.Interop;

// adm_output_formats_json / adm_capabilities_json 的 DTO + 查询。
// System.Text.Json 源生成器(JsonSerializerContext)→ AOT 安全,无运行时反射。
// 字段名 snake_case 由 SnakeCaseLower 策略映射;含数字/缩写的(is_3d、iamf_mp4_packager)显式 JsonPropertyName。

// ── mradm.output-formats v1 ──
internal sealed class OutputFormatsDoc
{
    public FeaturesDto Features { get; set; } = new();
    public List<FormatDto> Formats { get; set; } = new();
}

internal sealed class FeaturesDto
{
    public bool Apac { get; set; }
    public bool Iamf { get; set; }
    [JsonPropertyName("iamf_mp4_packager")] public bool IamfMp4Packager { get; set; }
    public bool Sofa { get; set; }
}

internal sealed class FormatDto
{
    public string Format { get; set; } = "";
    public List<string> Extensions { get; set; } = new();
    public bool Available { get; set; }
    public string? AvailableReason { get; set; }
    public bool Lossy { get; set; }
    public int MaxChannels { get; set; }
    public int FixedSampleRate { get; set; }
    public bool SupportsHeight { get; set; }
    public string? Note { get; set; }
    public List<string>? BitDepths { get; set; }
    [JsonPropertyName("bitrate_kbps_per_ch")] public BitrateDto? BitratePerCh { get; set; }
    [JsonPropertyName("bitrate_kbps_total")] public BitrateDto? BitrateTotal { get; set; }
}

internal sealed class BitrateDto
{
    public int Min { get; set; }
    public int Max { get; set; }
    public int Auto { get; set; }
}

// ── mradm.capabilities v1 ──
internal sealed class CapabilitiesDoc
{
    public List<BackendDto> Backends { get; set; } = new();

    // 平台系统空间音频 sink 接受的扬声器布局(macOS=ASBR / Windows=ISpatialAudioClient;不支持平台为空)。
    // 系统空间音频监听后端的布局候选以此为权威源,跨平台,GUI 不再硬编码白名单。
    public List<LayoutDto> SystemSpatialLayouts { get; set; } = new();

    // macOS 运行时自检:此 OS 的 AUSpatialMixer 是否把 LFE 正确路由到输出 LFE 声道。macOS ≤26.3 会误路由到
    // 中置 → Apple 渲染的系统空间床丢 LFE。字段仅 macOS core 输出;缺失(非 mac / 旧 core)默认 true 不告警。
    [JsonPropertyName("apple_system_spatial_lfe_routing_ok")]
    public bool AppleSystemSpatialLfeRoutingOk { get; set; } = true;
}

internal sealed class BackendDto
{
    public string Renderer { get; set; } = "";
    public string BackendName { get; set; } = "";
    public List<LayoutDto> Layouts { get; set; } = new();
}

internal sealed class LayoutDto
{
    public string Id { get; set; } = "";
    public string DisplayName { get; set; } = "";
    public int ChannelCount { get; set; }
    [JsonPropertyName("is_3d")] public bool Is3d { get; set; }
    public bool IsBinaural { get; set; }
}

// ── mradm.render-support-matrix v1 ──
// renderer × layout × target 笛卡尔积,每条带 supported + reason。
// layouts 用 display 名(binaural 单列,内部不再出现 0+2+0 Stereo);
// targets 区分 apac_mpeg4/apac_caf、iamf/iamf_mp4(带 required_option)。
// entries.supported 已内置全部约束(声道/高度/APAC 布局白名单),GUI 直接照用。
internal sealed class SupportMatrixDoc
{
    public FeaturesDto Features { get; set; } = new();
    public List<SmBackendDto> Backends { get; set; } = new();
    public List<SmLayoutDto> Layouts { get; set; } = new();
    public List<SmTargetDto> Targets { get; set; } = new();
    public List<SmEntryDto> Entries { get; set; } = new();
}

internal sealed class SmBackendDto
{
    public string Renderer { get; set; } = "";
    public string BackendName { get; set; } = "";
}

internal sealed class SmLayoutDto
{
    public string Layout { get; set; } = "";
    public string LayoutId { get; set; } = "";
    public int Channels { get; set; }
    [JsonPropertyName("is_3d")] public bool Is3d { get; set; }
}

internal sealed class SmTargetDto
{
    public string Target { get; set; } = "";
    public string Format { get; set; } = "";
    public string Container { get; set; } = "";
    public string Encoding { get; set; } = "";
    public List<string> Extensions { get; set; } = new();
    public bool Available { get; set; }
    public string? AvailableReason { get; set; }
    public bool Lossy { get; set; }
    public int MaxChannels { get; set; }
    public bool SupportsHeight { get; set; }
    public RequiredOptionDto? RequiredOption { get; set; }
}

internal sealed class RequiredOptionDto
{
    public string Name { get; set; } = "";
    public string Value { get; set; } = "";
}

internal sealed class SmEntryDto
{
    public string Renderer { get; set; } = "";
    public string Layout { get; set; } = "";
    public string Target { get; set; } = "";
    public bool Supported { get; set; }
}

// ── mradm.layouts v1(adm_layouts_json):每 (format, layout) 一行,order = 最终声道顺序(空格分隔)──
// 系统空间音频电平表用它取逐声道标签:CoreAudio 顺序(caf/apac 行)即 ASBR 监听实际输出顺序。
internal sealed class LayoutsDoc
{
    public List<LayoutOrderDto> Layouts { get; set; } = new();
}

internal sealed class LayoutOrderDto
{
    public string Format { get; set; } = "";
    public string Layout { get; set; } = "";
    public int Channels { get; set; }
    public string Order { get; set; } = "";
}

// ── adm_monitor_output_devices_json(v1.21):顶层数组 [{id,name,default}] ──
internal sealed class OutputDeviceDto
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public bool Default { get; set; }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower)]
[JsonSerializable(typeof(OutputFormatsDoc))]
[JsonSerializable(typeof(CapabilitiesDoc))]
[JsonSerializable(typeof(SupportMatrixDoc))]
[JsonSerializable(typeof(LayoutsDoc))]
[JsonSerializable(typeof(List<OutputDeviceDto>))]
internal partial class AdmJsonContext : JsonSerializerContext
{
}

/// <summary>调 C ABI JSON 查询接口并反序列化;任何失败返回 null(调用方回退硬编码)。</summary>
internal static class AdmQueries
{
    private delegate AdmErrorCode JsonQuery(AdmContextHandle ctx, out IntPtr outJson);

    public static OutputFormatsDoc? LoadOutputFormats(AdmContextHandle ctx) =>
        Load(ctx, NativeMethods.adm_output_formats_json, AdmJsonContext.Default.OutputFormatsDoc);

    public static CapabilitiesDoc? LoadCapabilities(AdmContextHandle ctx) =>
        Load(ctx, NativeMethods.adm_capabilities_json, AdmJsonContext.Default.CapabilitiesDoc);

    public static SupportMatrixDoc? LoadSupportMatrix(AdmContextHandle ctx) =>
        Load(ctx, NativeMethods.adm_render_support_matrix_json, AdmJsonContext.Default.SupportMatrixDoc);

    public static LayoutsDoc? LoadLayouts(AdmContextHandle ctx) =>
        Load(ctx, NativeMethods.adm_layouts_json, AdmJsonContext.Default.LayoutsDoc);

    public static List<OutputDeviceDto>? LoadOutputDevices(AdmContextHandle ctx) =>
        Load(ctx, NativeMethods.adm_monitor_output_devices_json, AdmJsonContext.Default.ListOutputDeviceDto);

    // 文件级 JSON 查询(adm_inspect_file_json / adm_policy_template_json):
    // 返回原始 JSON 字符串(强类型 DTO 在消费方各任务定义);失败返回 null。
    public static string? FetchInspectJson(AdmContextHandle ctx, string inputPath) =>
        FetchFileJson(NativeMethods.adm_inspect_file_json, ctx, inputPath);

    public static string? FetchPolicyTemplateJson(AdmContextHandle ctx, string inputPath) =>
        FetchFileJson(NativeMethods.adm_policy_template_json, ctx, inputPath);

    private delegate AdmErrorCode FileJsonQuery(AdmContextHandle ctx, string inputPath, out IntPtr outJson);

    private static string? FetchFileJson(FileJsonQuery query, AdmContextHandle ctx, string inputPath)
    {
        if (string.IsNullOrEmpty(inputPath) || query(ctx, inputPath, out var p) != AdmErrorCode.Ok ||
            p == IntPtr.Zero)
        {
            return null;
        }

        try
        {
            return Marshal.PtrToStringUTF8(p);
        }
        finally
        {
            NativeMethods.adm_free_string(p);
        }
    }

    private static T? Load<T>(AdmContextHandle ctx, JsonQuery query, JsonTypeInfo<T> typeInfo)
        where T : class
    {
        if (query(ctx, out var p) != AdmErrorCode.Ok || p == IntPtr.Zero)
        {
            return null;
        }

        try
        {
            string json = Marshal.PtrToStringUTF8(p) ?? string.Empty;
            return JsonSerializer.Deserialize(json, typeInfo);
        }
        catch (JsonException)
        {
            return null;
        }
        finally
        {
            NativeMethods.adm_free_string(p);
        }
    }
}
