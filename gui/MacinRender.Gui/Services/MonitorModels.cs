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

/// <summary>轮询所得的逐声道 peak / rms(adm_monitor_get_levels)。</summary>
public sealed record MonitorLevelsSnapshot(
    IReadOnlyList<float> Peak,
    IReadOnlyList<float> Rms);

/// <summary>一条按对象的实时覆盖。gain 即时;*_scale 为拓扑(binaural 经廉价 re-prepare,
/// 其它后端接受但忽略),默认 1.0 表示不变。</summary>
public sealed record MonitorOverride(
    string ObjectId,
    float GainDb = 0.0f,
    float DiffuseScale = 1.0f,
    float ExtentScale = 1.0f,
    float DivergenceScale = 1.0f);
