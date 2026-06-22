using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.ViewModels;

/// <summary>下拉里的一个 HRIR 选项:Path=null 表示内置默认(KEMAR);否则一个自定义 SOFA 文件。
/// 值相等只看 Path → 重建列表后能按路径重新选中(默认项 Path 恒为 null)。</summary>
public sealed record SofaOption(string? Path)
{
    public bool IsDefault => string.IsNullOrEmpty(Path);

    // 默认项标签走 i18n;自定义项=文件名(去扩展,中性专名,免翻,与其它专名下拉一致)。
    public string Label => IsDefault ? Localizer.Instance["SemSofaDefault"] : System.IO.Path.GetFileNameWithoutExtension(Path!);

    public string? Tooltip => Path;
}

/// <summary>
/// 可复用的 HRIR(SOFA)下拉选择器:列出「默认 KEMAR + 最近使用(MRU)」,选中即切换。浏览新文件
/// 经 Pick 加入共享 MRU。批渲染与监听各持一个实例,共享 SofaLibrary 的 MRU——一处选过的下次两处都
/// 在列表里(一点即换,免反复走文件对话框)。是纯 UI 便利层,真实路径仍由各 VM 的字段/下游消费。
/// </summary>
public sealed partial class SofaSelector : ObservableObject
{
    public ObservableCollection<SofaOption> Options { get; } = new();

    [ObservableProperty] private SofaOption _selected = new(null); // 构造时 Rebuild 立即覆盖(编译器看不穿,先给默认项占位)

    private bool _suppress; // 重建/编程式选中时抑制 Changed,避免回环

    /// <summary>用户切换了选择(含浏览新文件后);参数为新路径(null=默认)。</summary>
    public event Action<string?>? SelectionChanged;

    public SofaSelector(string? initialPath)
    {
        Rebuild(initialPath);
        SofaLibrary.Changed += OnLibraryChanged;          // 另一处加了新 SOFA → 同步刷新列表
        Localizer.Instance.PropertyChanged += (_, _) => Rebuild(CurrentPath); // 语言切换 → 默认项标签更新
    }

    public string? CurrentPath => Selected?.Path;

    partial void OnSelectedChanged(SofaOption value)
    {
        if (!_suppress)
        {
            SelectionChanged?.Invoke(value?.Path);
        }
    }

    /// <summary>浏览到新文件:提到 MRU 最前(广播 → 各 selector 重建),再选中它。</summary>
    public void Pick(string path)
    {
        SofaLibrary.Add(path);
        Select(path);
    }

    /// <summary>按路径选中已有项(无匹配则回落默认)。编程式,不抛 SelectionChanged。</summary>
    public void Select(string? path)
    {
        _suppress = true;
        Selected = Find(path);
        _suppress = false;
        SelectionChanged?.Invoke(CurrentPath); // 真实路径可能因回落而变,主动通知一次
    }

    private void OnLibraryChanged() => Rebuild(CurrentPath);

    // 用「默认 + 最近」重建下拉项,尽量保持当前路径选中。
    private void Rebuild(string? keepPath)
    {
        _suppress = true;
        Options.Clear();
        Options.Add(new SofaOption(null));
        foreach (var p in SofaLibrary.Recent())
        {
            Options.Add(new SofaOption(p));
        }

        Selected = Find(keepPath);
        _suppress = false;
    }

    private SofaOption Find(string? path) =>
        Options.FirstOrDefault(o => string.Equals(o.Path, path, StringComparison.OrdinalIgnoreCase)) ?? Options[0];
}
