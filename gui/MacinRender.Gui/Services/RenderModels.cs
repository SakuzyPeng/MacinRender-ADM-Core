using System.Collections.Generic;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Services;

/// <summary>一次渲染的输入设置(托管侧),由 AdmRenderService 翻译成 options setter 调用。</summary>
public sealed record RenderSettings
{
    public AdmRenderer Renderer { get; init; } = AdmRenderer.Automatic;
    public string Layout { get; init; } = "binaural";
    public AdmOutputBitDepth? BitDepth { get; init; }
    public uint? OpusBitratePerChKbps { get; init; }
    public uint? ApacBitrateKbps { get; init; }
    public AdmApacContainer? ApacContainer { get; init; }
    public double? LoudnessTargetLufs { get; init; }
    public string? SofaPath { get; init; }
    public string? SemanticPolicyJson { get; init; }
    public AdmSpeakerGeometry SpeakerGeometry { get; init; } = AdmSpeakerGeometry.Standard;
    public AdmLfeRoutingMode LfeRoutingMode { get; init; } = AdmLfeRoutingMode.Direct;

    /// <summary>仅监听:多声道输出走 macOS 系统空间音频(AVSampleBufferAudioRenderer,系统 HRTF + 头追踪),
    /// 不下混。需受支持的多声道扬声器 Layout;非 macOS / 离线渲染忽略。</summary>
    public bool MonitorSystemSpatial { get; init; }

    internal static bool Is22Point2Layout(string? layout) =>
        string.Equals(layout, "22.2", System.StringComparison.OrdinalIgnoreCase) ||
        string.Equals(layout, "9+10+3", System.StringComparison.OrdinalIgnoreCase);

    /// <summary>避免 GUI 保留的 22.2 偏好在其它布局上触发无意义 warning。</summary>
    internal AdmLfeRoutingMode EffectiveLfeRoutingMode =>
        Is22Point2Layout(Layout) ? LfeRoutingMode : AdmLfeRoutingMode.Direct;
}

/// <summary>结构化进度事件(adm_progress_event_v2_t 的托管投影,Message 已 marshal 成托管串)。</summary>
public sealed record RenderProgress(
    double OverallFraction,
    AdmRenderStage Stage,
    AdmProgressOperation Operation,
    string? Message);

/// <summary>一条渲染诊断日志(对应 adm_render_result_log_entry)。</summary>
public sealed record RenderLogEntry(AdmLogLevel Level, string Module, string Message);

/// <summary>渲染结果汇总。指标缺失(静音 / 信号过短 / 未测量)时对应字段为 null。</summary>
public sealed record RenderOutcome(
    bool Success,
    AdmErrorCode ErrorCode,
    string Message,
    string? OutputPath,
    double? LoudnessLufs,
    double? PeakDbtp,
    IReadOnlyList<RenderLogEntry> Logs);
