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

        int need = Math.Max(2, (int) Math.Ceiling(names.Count * 0.4));
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

// adm_inspect_file_json (schema "mradm.scene-inspect" v1) 的部分 DTO —— 只取语义编辑
// 需要的字段(对象 + 首块的听感维度当前值)。System.Text.Json 源生成器,AOT 安全。
// 字段 snake_case 由 SnakeCaseLower 策略映射;dialogue_id / track_uid / object_blocks 等自动对上。

internal sealed class InspectDoc
{
    public List<InspectObject> Objects { get; set; } = new();
}

internal sealed class InspectObject
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public double Gain { get; set; } = 1.0;
    public bool Mute { get; set; }
    public int? Importance { get; set; }
    public int? DialogueId { get; set; }
    public List<InspectTrack> Tracks { get; set; } = new();
}

internal sealed class InspectTrack
{
    public string TrackUid { get; set; } = "";
    public List<InspectObjectBlock> ObjectBlocks { get; set; } = new();
}

internal sealed class InspectObjectBlock
{
    public double Gain { get; set; } = 1.0;
    public double Diffuse { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }
    public double Depth { get; set; }
    public double Divergence { get; set; }
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
