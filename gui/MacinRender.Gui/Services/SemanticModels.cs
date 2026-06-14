using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacinRender.Gui.Services;

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
