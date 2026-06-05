using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;

namespace MacinRender.Gui.Models;

public enum SubOption
{
    None,
    BitDepth,
    BitratePerCh,
    BitrateTotal
}

public sealed record LayoutDef(string Id, string Name, int Channels, bool HasHeight);

public sealed record BackendDef(string Id, string Name, IReadOnlyList<string> LayoutIds);

public sealed record ContainerDef(string Id, string Name, IReadOnlyList<string> BitDepths);

public sealed record CodecDef(
    string Id,
    string Name,
    IReadOnlyList<string> ContainerIds,
    int MaxChannels,
    int FixedSampleRate,
    bool SupportsHeight,
    bool Available,
    string Reason,
    SubOption Sub,
    // 非 null = 仅支持这些布局(白名单,如 APAC);null = 用 MaxChannels/SupportsHeight 约束
    IReadOnlyList<string>? AllowedLayoutIds = null);

/// <summary>一个编码器在「当前布局」下的可选状态(含禁用原因标签)。</summary>
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
/// 真实输出格式模型(硬编码自 `mradm formats` + `mradm backends`)。
/// 花瓶用静态数据;接 C ABI 后换成 adm_output_formats_json + backends JSON,规则一致。
/// </summary>
public static class OutputModel
{
    // 花瓶硬编码;接 ABI 时读 features.apac / features.iamf。
    public const bool ApacAvailable = true;
    public const bool IamfAvailable = false;

    public static readonly LayoutDef[] AllLayouts =
    {
        new("binaural", "Binaural", 2, false),
        new("5.1", "5.1", 6, false),
        new("7.1", "7.1", 8, false),
        new("5.1.2", "5.1.2", 8, true),
        new("5.1.4", "5.1.4", 10, true),
        new("7.1.4", "7.1.4", 12, true),
        new("9.1.4", "9.1.4", 14, true),
        new("9.1.6", "9.1.6", 16, true),
        new("22.2", "22.2", 24, true),
        new("hoa3", "HOA 3 阶", 16, true),
    };

    public static readonly Dictionary<string, LayoutDef> LayoutById = AllLayouts.ToDictionary(l => l.Id);

    private static readonly string[] SpeakerLayouts =
        { "5.1", "5.1.2", "5.1.4", "7.1", "7.1.4", "9.1.4", "9.1.6", "22.2" };

    private static readonly string[] AppleLayouts =
        { "binaural", "5.1", "7.1", "5.1.2", "5.1.4", "7.1.4", "9.1.6", "22.2" };

    public static readonly ObservableCollection<BackendDef> Backends = new()
    {
        new("automatic", "自动", AllLayouts.Select(l => l.Id).ToArray()),
        new("ear", "EAR (BS.2127)", SpeakerLayouts),
        new("vbap", "VBAP", SpeakerLayouts),
        new("hoa", "HOA", new[] { "hoa3" }),
        new("binaural", "双耳", new[] { "binaural" }),
        new("apple", "Apple SpatialMixer", AppleLayouts),
    };

    public static readonly Dictionary<string, BackendDef> BackendById = Backends.ToDictionary(b => b.Id);

    public static readonly Dictionary<string, ContainerDef> ContainerById = new()
    {
        ["wav"] = new("wav", "WAV (.wav)", new[] { "f32" }), // 暂时固定 f32
        ["caf"] = new("caf", "CAF (.caf)", new[] { "f32" }),
        ["flac"] = new("flac", "FLAC (.flac)", Array.Empty<string>()),
        ["mka"] = new("mka", "Matroska (.mka)", Array.Empty<string>()),
        ["apac_m4a"] = new("apac_m4a", "MPEG-4 (.m4a)", Array.Empty<string>()),
        ["apac_mp4"] = new("apac_mp4", "MPEG-4 (.mp4)", Array.Empty<string>()),
        ["apac_caf"] = new("apac_caf", "CAF (.caf)", Array.Empty<string>()),
        ["iamf_obu"] = new("iamf_obu", "OBU (.iamf)", Array.Empty<string>()),
        ["iamf_mp4"] = new("iamf_mp4", "MP4 (.mp4)", Array.Empty<string>()),
    };

    public static readonly CodecDef[] Codecs =
    {
        new("pcm", "PCM(未压缩)", new[] { "wav", "caf" }, int.MaxValue, 0, true, true, "", SubOption.None),
        new("flac", "FLAC(无损)", new[] { "flac" }, 8, 0, false, true, "", SubOption.None),
        new("opus", "Opus(有损)", new[] { "mka" }, 255, 48000, true, true, "", SubOption.BitratePerCh),
        new("apac", "APAC(有损)", new[] { "apac_m4a", "apac_mp4", "apac_caf" }, 24, 48000, true, ApacAvailable,
            "macOS 专用", SubOption.BitrateTotal,
            // APAC 实际布局白名单(apac_io.cpp);不支持 5.1 / 5.1.2 / 9.1.4
            new[] { "binaural", "7.1", "5.1.4", "7.1.4", "9.1.6", "22.2", "hoa3" }),
        new("iamf", "IAMF(有损)", new[] { "iamf_obu", "iamf_mp4" }, 12, 48000, true, IamfAvailable,
            "需 IAMF 构建", SubOption.None),
    };
}
