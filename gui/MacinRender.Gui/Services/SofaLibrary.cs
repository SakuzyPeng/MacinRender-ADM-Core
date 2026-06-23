using System;
using System.Collections.Generic;
using System.IO;

namespace MacinRender.Gui.Services;

/// <summary>
/// 自定义 HRIR(SOFA)最近使用列表(MRU)。批渲染与监听共享同一份,存在 settings.json 的
/// RecentSofaPaths(最近在前)。只存路径;展示时过滤掉已不存在的文件。Changed 在列表变化时触发,
/// 让所有正打开的下拉同步刷新。读写经 SettingsStore.Update(读-改-写,绝不覆盖其它字段)。
/// </summary>
public static class SofaLibrary
{
    private const int MaxRecent = 6;

    /// <summary>MRU 变化(新增 / 重排)。各 SofaSelector 订阅以重建下拉项。</summary>
    public static event Action? Changed;

    /// <summary>当前最近列表(最近在前),仅含仍存在的文件。</summary>
    public static IReadOnlyList<string> Recent()
    {
        var stored = SettingsStore.Load()?.RecentSofaPaths;
        if (stored is null)
        {
            return Array.Empty<string>();
        }

        var result = new List<string>(stored.Count);
        foreach (var p in stored)
        {
            if (!string.IsNullOrEmpty(p) && File.Exists(p))
            {
                result.Add(p);
            }
        }

        return result;
    }

    /// <summary>把一个 SOFA 提到 MRU 最前(去重、剔除已失踪项、限长),并广播 Changed。</summary>
    public static void Add(string path)
    {
        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        SettingsStore.Update(s =>
        {
            var list = s.RecentSofaPaths ?? new List<string>();
            // 去重(忽略大小写)+ 剔除已不存在的(刚选中的必然存在)。
            list.RemoveAll(p => string.IsNullOrEmpty(p) ||
                (!string.Equals(p, path, StringComparison.OrdinalIgnoreCase) && !File.Exists(p)) ||
                string.Equals(p, path, StringComparison.OrdinalIgnoreCase));
            list.Insert(0, path);
            if (list.Count > MaxRecent)
            {
                list.RemoveRange(MaxRecent, list.Count - MaxRecent);
            }

            s.RecentSofaPaths = list;
        });

        Changed?.Invoke();
    }
}
