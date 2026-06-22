using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MacinRender.Gui.Services;

/// <summary>持久化的用户选择(下次启动恢复)。字段存稳定 id/标志,恢复时在当前支持矩阵里匹配,失配则忽略。</summary>
public sealed class AppSettings
{
    public string? Backend { get; set; }
    public string? Codec { get; set; }
    public string? Layout { get; set; }
    public string? Container { get; set; }
    public decimal? Bitrate { get; set; }
    public bool IsDark { get; set; } = true;
    public bool IsEnglish { get; set; }
    public string? SofaPath { get; set; } // 批渲染自定义 HRIR(SOFA)路径(当前选择)
    public string? MonitorSofaPath { get; set; } // 语义监听自定义 HRIR(SOFA)路径(当前选择)
    public List<string>? RecentSofaPaths { get; set; } // 最近用过的 SOFA(MRU,最近在前;批渲染 + 监听共享)
    public string? MonitorDeviceId { get; set; } // 监听输出设备 token(空 = 系统默认)
}

// source generator:AOT/trim 安全,无运行时反射序列化。
[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(AppSettings))]
internal partial class SettingsJsonContext : JsonSerializerContext
{
}

/// <summary>
/// 设置读写:~/.config/MacinRender ADM/settings.json(跨平台经 SpecialFolder.ApplicationData)。
/// 读写失败一律静默——设置持久化是锦上添花,绝不阻断主流程。
/// </summary>
public static class SettingsStore
{
    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "MacinRender ADM", "settings.json");

    public static AppSettings? Load()
    {
        try
        {
            var path = FilePath;
            if (!File.Exists(path))
            {
                return null;
            }

            return JsonSerializer.Deserialize(File.ReadAllText(path), SettingsJsonContext.Default.AppSettings);
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static void Save(AppSettings settings)
    {
        try
        {
            var path = FilePath;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, JsonSerializer.Serialize(settings, SettingsJsonContext.Default.AppSettings));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // 持久化失败不影响功能。
        }
    }

    public static void Update(Action<AppSettings> update)
    {
        var settings = Load() ?? new AppSettings();
        update(settings);
        Save(settings);
    }
}
