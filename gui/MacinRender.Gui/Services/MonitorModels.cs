using System.Collections.Generic;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Services;

/// <summary>轮询所得的播放状态快照(adm_monitor_get_status 的托管投影)。</summary>
public sealed record MonitorStatusSnapshot(
    AdmMonitorState State,
    ulong PlayheadFrames,
    ulong Underruns,
    ulong BufferedFrames,
    float RingFill,
    bool Ended,
    bool Failed,
    ulong OverrideRevision);

/// <summary>轮询所得的逐声道 peak / rms 及监听输出的整体 LUFS(adm_monitor_get_levels,v1.18)。
/// LUFS 为 ITU-R BS.1770 响度;静音 / 低于门限时为 -inf。</summary>
public sealed record MonitorLevelsSnapshot(
    IReadOnlyList<float> Peak,
    IReadOnlyList<float> Rms,
    float MomentaryLufs,
    float ShorttermLufs,
    float IntegratedLufs);

/// <summary>一条按对象的实时覆盖。gain 即时;*_scale 为拓扑(binaural 经廉价 re-prepare,
/// 其它后端接受但忽略),默认 1.0 表示不变。</summary>
public sealed record MonitorOverride(
    string ObjectId,
    float GainDb = 0.0f,
    float DiffuseScale = 1.0f,
    float ExtentScale = 1.0f,
    float DivergenceScale = 1.0f);

/// <summary>监听后端 A/B 选项:展示名 + 渲染器 + 监听布局。立体声监听下可跨布局(经下混)。</summary>
public sealed record MonitorBackendOption(string Label, AdmRenderer Renderer, string Layout);
