using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Models;

public enum SubOption
{
    None,
    BitratePerCh,
    BitrateTotal
}

public sealed record LayoutDef(string Id, string Name, int Channels, bool HasHeight);

// Renderer = 渲染时传 adm_render_options_set_renderer 的枚举;Id = support-matrix 的 renderer 字符串。
public sealed record BackendDef(string Id, string Name, IReadOnlyList<string> LayoutIds, AdmRenderer Renderer);

// Id = support-matrix 的 target;Ext = 输出文件扩展名(无点)。
public sealed record ContainerDef(string Id, string Name, string Ext);

// Id = support-matrix 的 encoding(pcm/flac/opus/apac)。
public sealed record CodecDef(string Id, string Name, SubOption Sub, bool Available, string Reason);

/// <summary>一个编码器在「当前后端 + 布局」下的可选状态(含禁用原因标签)。</summary>
public sealed class CodecOption
{
    public CodecOption(CodecDef def, bool isEnabled, string label)
    {
        Def = def;
        IsEnabled = isEnabled;
        Label = label;
    }

    public CodecDef Def { get; }
    public bool IsEnabled { get; }
    public string Label { get; }
}

/// <summary>
/// 输出模型,由 adm_render_support_matrix_json(v1.12)驱动:
///   - layouts/backends/targets 全部来自 support-matrix(display 名,binaural 为唯一 2ch,无 Stereo)
///   - 联动用 entries.supported 索引,已内置声道/高度/APAC 布局白名单等全部约束
/// IAMF target 暂不暴露(GUI 放弃支持)。ABI 是唯一数据源;加载失败时应用应中止启动。
/// </summary>
public static class OutputModel
{
    public static bool ApacAvailable { get; private set; }
    public static bool IamfAvailable { get; private set; }
    public static bool SofaAvailable { get; private set; }

    public static ObservableCollection<BackendDef> Backends { get; private set; } = new();
    public static Dictionary<string, BackendDef> BackendById { get; private set; } = new();
    public static Dictionary<string, LayoutDef> LayoutById { get; private set; } = new();
    public static Dictionary<string, ContainerDef> ContainerById { get; private set; } = new();
    public static CodecDef[] Codecs { get; private set; } = Array.Empty<CodecDef>();

    // (renderer, layout, target) → 受支持。"automatic" 为所有真实后端的并集。
    private static HashSet<(string Renderer, string Layout, string Target)> _supported = new();
    // codec(encoding) → 该编码的 target id(有序);不含 iamf。
    private static Dictionary<string, string[]> _codecTargets = new();

    /// <summary>该 (后端, 布局) 下编码器是否有任一容器受支持。</summary>
    public static bool IsCodecSupported(string renderer, string layout, string codecId) =>
        _codecTargets.TryGetValue(codecId, out var targets) &&
        targets.Any(t => _supported.Contains((renderer, layout, t)));

    /// <summary>该后端在「任一受支持布局」下是否支持该编码器(决定编码器是否进入列表)。</summary>
    public static bool IsCodecSupportedByBackend(BackendDef backend, string codecId) =>
        backend.LayoutIds.Any(l => IsCodecSupported(backend.Id, l, codecId));

    /// <summary>该 (后端, 布局, 编码器) 下受支持的容器 target id(有序)。</summary>
    public static IEnumerable<string> SupportedContainers(string renderer, string layout, string codecId)
    {
        if (!_codecTargets.TryGetValue(codecId, out var targets))
        {
            yield break;
        }

        foreach (var t in targets)
        {
            if (_supported.Contains((renderer, layout, t)))
            {
                yield return t;
            }
        }
    }

    internal static void Initialize(SupportMatrixDoc? matrix)
    {
        if (matrix is not { Layouts.Count: > 0, Backends.Count: > 0, Targets.Count: > 0, Entries.Count: > 0 })
        {
            throw new InvalidOperationException("adm_render_support_matrix_json returned an empty or invalid matrix.");
        }

        BuildFromMatrix(matrix);

        if (Backends.Count == 0 || LayoutById.Count == 0 || ContainerById.Count == 0 || Codecs.Length == 0)
        {
            throw new InvalidOperationException("adm_render_support_matrix_json did not provide a usable GUI model.");
        }
    }

    private static void BuildFromMatrix(SupportMatrixDoc m)
    {
        ApacAvailable = m.Features.Apac;
        IamfAvailable = m.Features.Iamf;
        SofaAvailable = m.Features.Sofa;

        LayoutById = m.Layouts.ToDictionary(l => l.Layout,
            l => new LayoutDef(l.Layout, FriendlyLayout(l.Layout), l.Channels, l.Is3d));

        // 支持索引(+ automatic 并集)。
        var supported = new HashSet<(string, string, string)>();
        foreach (var e in m.Entries)
        {
            if (e.Supported)
            {
                supported.Add((e.Renderer, e.Layout, e.Target));
                supported.Add(("automatic", e.Layout, e.Target));
            }
        }

        _supported = supported;

        // targets → 容器 + codec(encoding) 分组;跳过 IAMF。
        ContainerById = new Dictionary<string, ContainerDef>();
        var codecTargets = new Dictionary<string, List<string>>();
        foreach (var t in m.Targets)
        {
            var codecId = EncodingToCodec(t.Encoding);
            if (codecId is null)
            {
                continue;
            }

            var ext = t.Extensions.FirstOrDefault()?.TrimStart('.') ?? "wav";
            ContainerById[t.Target] = new ContainerDef(t.Target, ContainerName(t.Target), ext);
            if (!codecTargets.TryGetValue(codecId, out var list))
            {
                list = new List<string>();
                codecTargets[codecId] = list;
            }

            list.Add(t.Target);
        }

        _codecTargets = codecTargets.ToDictionary(k => k.Key, k => k.Value.ToArray());
        Codecs = BuildCodecs(m);

        // backends:每 renderer 实际受支持的布局(从 entries)。
        var backends = new ObservableCollection<BackendDef>();
        foreach (var b in m.Backends)
        {
            var layoutIds = m.Layouts.Select(l => l.Layout)
                .Where(l => m.Entries.Any(e => e.Renderer == b.Renderer && e.Layout == l && e.Supported))
                .ToList();
            if (layoutIds.Count > 0)
            {
                backends.Add(new BackendDef(b.Renderer, FriendlyBackend(b.Renderer, b.BackendName), layoutIds,
                    MapRenderer(b.Renderer)));
            }
        }

        var allLayouts = m.Layouts.Select(l => l.Layout).Where(l => backends.Any(b => b.LayoutIds.Contains(l))).ToList();
        backends.Insert(0, new BackendDef("automatic", "自动", allLayouts, AdmRenderer.Automatic));

        Backends = backends;
        BackendById = backends.ToDictionary(b => b.Id);
    }

    private static CodecDef[] BuildCodecs(SupportMatrixDoc m)
    {
        var list = new List<CodecDef>();

        void AddIfPresent(string id, string name, SubOption sub, string offReason)
        {
            if (!_codecTargets.TryGetValue(id, out var targets))
            {
                return;
            }

            var available = targets.Any(t => m.Targets.First(x => x.Target == t).Available);
            list.Add(new CodecDef(id, name, sub, available, available ? "" : offReason));
        }

        AddIfPresent("pcm", "PCM(未压缩)", SubOption.None, "");
        AddIfPresent("flac", "FLAC(无损)", SubOption.None, "");
        AddIfPresent("opus", "Opus(有损)", SubOption.BitratePerCh, "不可用");
        AddIfPresent("apac", "APAC(有损)", SubOption.BitrateTotal, "macOS 专用");
        return list.ToArray();
    }

    private static string? EncodingToCodec(string encoding) => encoding switch
    {
        "pcm" => "pcm",
        "flac" => "flac",
        "opus" => "opus",
        "apac" => "apac",
        _ => null, // iamf_opus 等暂不暴露
    };

    private static string ContainerName(string target) => target switch
    {
        "wav" => "WAV (.wav)",
        "caf" => "CAF (.caf)",
        "flac" => "FLAC (.flac)",
        "opus_mka" => "Matroska (.mka)",
        "apac_mpeg4" => "MPEG-4 (.m4a)",
        "apac_caf" => "CAF (.caf)",
        _ => target,
    };

    // 对齐 CLI backend_name 语义:saf-vbap / saf-binaural-hrtf 都是 SAF 实现,名字体现出来。
    private static string FriendlyBackend(string renderer, string backendName) => renderer switch
    {
        "ear" => "EAR (BS.2127)",
        "saf" => "SAF VBAP",
        "hoa" => "HOA",
        "saf-binaural" => "SAF 双耳",
        "apple" => "Apple SpatialMixer",
        _ => string.IsNullOrEmpty(backendName) ? renderer : backendName,
    };

    private static string FriendlyLayout(string layout) => layout switch
    {
        "binaural" => "Binaural",
        "hoa3" => "HOA 3 阶",
        _ => layout,
    };

    private static AdmRenderer MapRenderer(string renderer) => renderer switch
    {
        "ear" => AdmRenderer.Ear,
        "saf" => AdmRenderer.Saf,
        "hoa" => AdmRenderer.Hoa,
        "saf-binaural" => AdmRenderer.SafBinaural,
        "apple" => AdmRenderer.Apple,
        _ => AdmRenderer.Automatic,
    };

}
