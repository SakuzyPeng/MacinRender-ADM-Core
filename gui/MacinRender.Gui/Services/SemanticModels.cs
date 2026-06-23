using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacinRender.Gui.Services;

/// <summary>
/// 对象名展示处理。ADM 母版常把"曲名/文件名前缀_ + stem + 声道"作为对象名(DAW 导出),
/// 前缀是噪声、区分信息在后段。检测被多数对象共享、切在分隔符边界的"主前缀",展示时剥离
/// (全名仍由 tooltip 给)。同文件混入的短默认名(如"音频 6 L")不匹配该前缀,原样保留。
/// </summary>
internal static class ObjectNaming
{
    private const int MinPrefixLength = 4;

    /// <summary>多数对象共享、切在分隔符(_ - 空格 /)边界的最长主前缀;无则返回空串。</summary>
    public static string DetectCommonPrefix(IReadOnlyList<string> names)
    {
        if (names.Count < 2)
        {
            return "";
        }

        var counts = new Dictionary<string, int>();
        foreach (var name in names)
        {
            foreach (var prefix in BoundaryPrefixes(name))
            {
                counts[prefix] = counts.TryGetValue(prefix, out var c) ? c + 1 : 1;
            }
        }

        int need = Math.Max(2, (int)Math.Ceiling(names.Count * 0.4));
        string best = "";
        foreach (var (prefix, count) in counts)
        {
            if (count >= need && prefix.Length >= MinPrefixLength && prefix.Length > best.Length)
            {
                best = prefix;
            }
        }

        return best;
    }

    /// <summary>
    /// 拆出末尾声道:名字以(空格/-/_)+ L|R 结尾时,返回(去声道的 stem, 'L'|'R');否则(原名, '\0')。
    /// 要求 L/R 前必须有分隔符,避免误伤 "PEARL" 之类。用于 L/R 配对(一对默认同步编辑)。
    /// </summary>
    public static (string Stem, char Channel) SplitChannel(string name)
    {
        if (name.Length >= 2)
        {
            char last = char.ToUpperInvariant(name[^1]);
            if ((last == 'L' || last == 'R') && name[^2] is ' ' or '-' or '_')
            {
                return (name[..^2], last);
            }
        }

        return (name, '\0');
    }

    /// <summary>剥掉主前缀;若不以该前缀开头或剥后为空,返回原名。</summary>
    public static string Strip(string name, string prefix) =>
        prefix.Length > 0 && name.StartsWith(prefix, StringComparison.Ordinal) && name.Length > prefix.Length
            ? name[prefix.Length..]
            : name;

    private static IEnumerable<string> BoundaryPrefixes(string s)
    {
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] is '_' or '-' or ' ' or '/')
            {
                yield return s[..(i + 1)];
            }
        }
    }
}

// adm_inspect_file_json (schema "mradm.scene-inspect" v1) 的部分 DTO —— 取语义编辑 + 空间视图
// 需要的字段。System.Text.Json 源生成器,AOT 安全。
// 字段 snake_case 由 SnakeCaseLower 策略映射;dialogue_id / track_uid / object_blocks /
// start_sample / interp_length_samples 等自动对上。

internal sealed class InspectDoc
{
    public InspectFile? File { get; set; }
    public List<InspectObject> Objects { get; set; } = new();
}

// 文件级时间基准:空间视图用 num_frames/sample_rate 把样本位置换算成秒、对齐播放头。
internal sealed class InspectFile
{
    public uint SampleRate { get; set; }
    public ulong NumFrames { get; set; }
    public double DurationSeconds { get; set; }
}

internal sealed class InspectObject
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public double Gain { get; set; } = 1.0;
    public bool Mute { get; set; }
    public int? Importance { get; set; }
    public int? DialogueId { get; set; }
    public ulong? EndSample { get; set; } // 省略 = 活到片尾
    public List<InspectTrack> Tracks { get; set; } = new();
}

internal sealed class InspectTrack
{
    public string TrackUid { get; set; } = "";
    public List<InspectObjectBlock> ObjectBlocks { get; set; } = new();
    public List<InspectDsBlock> DsBlocks { get; set; } = new();
}

// DirectSpeakers block: label + current gain + position. Position pairs L/R by opposite azimuth
// (robust vs label strings) for the bed editor, and places static bed points in the spatial view.
internal sealed class InspectDsBlock
{
    public List<string> SpeakerLabels { get; set; } = new();
    public double Gain { get; set; } = 1.0;
    public bool HasPosition { get; set; }
    public double Azimuth { get; set; }
    public double Elevation { get; set; }
    public double Distance { get; set; } = 1.0;
}

// 位置:cartesian=true 用 x/y/z,否则用 azimuth/elevation/distance(scene_to_json 二选一序列化)。
internal sealed class InspectPosition
{
    public bool Cartesian { get; set; }
    public double X { get; set; }
    public double Y { get; set; }
    public double Z { get; set; }
    public double Azimuth { get; set; }
    public double Elevation { get; set; }
    public double Distance { get; set; } = 1.0;
}

internal sealed class InspectObjectBlock
{
    public double Gain { get; set; } = 1.0;
    public double Diffuse { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }
    public double Depth { get; set; }
    public double Divergence { get; set; }

    // 空间视图:位置 + 时间窗 + 插值线索(轨迹关键帧)。
    public InspectPosition? Position { get; set; }
    public ulong StartSample { get; set; }
    public ulong? EndSample { get; set; } // 省略 = 到片尾
    public bool JumpPosition { get; set; }
    public ulong? InterpLengthSamples { get; set; }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower)]
[JsonSerializable(typeof(InspectDoc))]
internal partial class SemanticJsonContext : JsonSerializerContext
{
}

internal static class SemanticInspect
{
    /// <summary>解析 inspect JSON;失败返回 null。</summary>
    public static InspectDoc? Parse(string? json)
    {
        if (string.IsNullOrEmpty(json))
        {
            return null;
        }

        try
        {
            return JsonSerializer.Deserialize(json, SemanticJsonContext.Default.InspectDoc);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}
