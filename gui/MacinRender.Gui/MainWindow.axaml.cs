using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Input.Platform;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Models;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui;

public partial class MainWindow : Window
{
    private static readonly string[] AdmExtensions = { ".wav", ".bw64", ".adm", ".rf64" };

    public MainWindow()
    {
        InitializeComponent();
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
        AddHandler(DragDrop.DragLeaveEvent, OnDragLeave);
        AddHandler(DragDrop.DropEvent, OnDrop);
        // 隧道阶段抢在子控件前看键:监听中用 ←/→ 快退/快进 5 秒(文本框内不拦,留给编辑光标)。
        AddHandler(KeyDownEvent, OnGlobalKeyDown, RoutingStrategies.Tunnel);
    }

    private const double SeekStepSeconds = 5.0;

    private void OnGlobalKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key is not (Key.Left or Key.Right))
        {
            return;
        }

        // 文本框内方向键留给编辑(移动光标),不抢。
        if (FocusManager?.GetFocusedElement() is TextBox)
        {
            return;
        }

        if (DataContext is not MainWindowViewModel { SemanticEditor: { IsMonitoring: true } se })
        {
            return;
        }

        se.SeekRelative(e.Key == Key.Left ? -SeekStepSeconds : SeekStepSeconds);
        e.Handled = true;
    }

    // 关窗时停掉实时监听,确保音频设备 + worker 干净收尾(否则等 SafeHandle 终结器不可靠)。
    protected override void OnClosed(EventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.SemanticEditor.StopMonitor();
        }

        base.OnClosed(e);
    }

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        bool ok = e.DataTransfer.Contains(DataFormat.File) &&
            DataContext is MainWindowViewModel { IsRendering: false, SemanticEditor.IsLoading: false };
        e.DragEffects = ok ? DragDropEffects.Copy : DragDropEffects.None;
        DropHighlight.Opacity = ok ? 1 : 0;
    }

    private void OnDragLeave(object? sender, DragEventArgs e) => DropHighlight.Opacity = 0;

    private async void OnDrop(object? sender, DragEventArgs e)
    {
        DropHighlight.Opacity = 0;
        if (DataContext is not MainWindowViewModel vm)
        {
            return;
        }

        // 顶层拖入项:文件 + 文件夹混合都收下原始路径,展开延到后台。
        var roots = e.DataTransfer.TryGetFiles()?
            .Select(f => f.TryGetLocalPath())
            .Where(p => !string.IsNullOrEmpty(p))
            .Select(p => p!)
            .ToList();
        if (roots is not { Count: > 0 })
        {
            return;
        }

        // 大文件夹递归可能耗时,放后台线程,避免卡 UI。批渲染交给 VM 再做 probe 内容级判定;
        // 语义编辑只载入第一个 ADM 候选。
        var paths = await Task.Run(() => ExpandAdmFiles(roots));
        if (vm.IsSemanticMode)
        {
            if (paths.Count > 0)
            {
                await vm.SemanticEditor.LoadFileAsync(paths[0]);
            }
            return;
        }

        await vm.AddFilesAsync(paths);
    }

    // 展开拖入项:文件直接收;文件夹递归遍历所有子目录找 ADM 文件。保持稳定顺序 + 去重。
    private static List<string> ExpandAdmFiles(IEnumerable<string> roots)
    {
        var result = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in roots)
        {
            if (Directory.Exists(root))
            {
                IEnumerable<string> files;
                try
                {
                    files = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
                        .Where(IsAdmFile)
                        .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                        .ToList();
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    continue;
                }

                foreach (var f in files)
                {
                    if (seen.Add(f))
                    {
                        result.Add(f);
                    }
                }
            }
            else if (File.Exists(root) && IsAdmFile(root) && seen.Add(root))
            {
                result.Add(root);
            }
        }

        return result;
    }

    private static bool IsAdmFile(string path) =>
        AdmExtensions.Contains(Path.GetExtension(path).ToLowerInvariant());

    // 选自定义 HRIR(SOFA)文件 → 交 VM(BuildSettings 在 SAF 双耳后端时带上)。
    private async void OnPickSofa(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
        {
            return;
        }

        var path = await PickSofaAsync(this);
        if (!string.IsNullOrEmpty(path))
        {
            vm.SetSofa(path);
            FlashIcon(FlashSofaPick);
        }
    }

    // 清除 SOFA(按钮仅在已选时可见 → 必然成功),闪绿确认。Click 与 ClearSofaCommand 并行触发。
    private void OnClearSofaClick(object? sender, RoutedEventArgs e) => FlashIcon(FlashSofaClear);

    // 共用的 SOFA 选择器:返回本地路径或 null。
    internal static async Task<string?> PickSofaAsync(Control owner)
    {
        var top = TopLevel.GetTopLevel(owner);
        if (top is null)
        {
            return null;
        }

        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = Localizer.Instance["SemSofaPick"],
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("SOFA HRIR") { Patterns = new[] { "*.sofa" } },
                FilePickerFileTypes.All,
            },
        });

        return files.Count > 0 ? files[0].TryGetLocalPath() : null;
    }

    private void OnTitleBarPressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            BeginMoveDrag(e);
        }
    }

    private async void OnAddFiles(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
        {
            return;
        }

        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = Localizer.Instance["PickTitle"],
            AllowMultiple = true,
            FileTypeFilter = new[]
            {
                new FilePickerFileType(Localizer.Instance["PickFilter"]) { Patterns = new[] { "*.wav", "*.bw64", "*.adm" } },
                FilePickerFileTypes.All,
            },
        });

        if (files.Count == 0)
        {
            return;
        }

        var paths = files
            .Select(f => f.TryGetLocalPath())
            .Where(p => !string.IsNullOrEmpty(p) && IsAdmFile(p!))
            .Select(p => p!)
            .ToList();
        if (await vm.AddFilesAsync(paths) > 0)
        {
            FlashIcon(FlashAddFile);
        }
    }

    private async void OnAddFolder(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
        {
            return;
        }

        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = Localizer.Instance["PickFolderTitle"],
            AllowMultiple = true,
        });

        var roots = folders
            .Select(f => f.TryGetLocalPath())
            .Where(p => !string.IsNullOrEmpty(p))
            .Select(p => p!)
            .ToList();
        if (roots.Count == 0)
        {
            return;
        }

        // 复用拖拽那套:递归展开 ADM 文件(后台),再交 VM 做 probe 内容级判定。
        var paths = await Task.Run(() => ExpandAdmFiles(roots));
        if (await vm.AddFilesAsync(paths) > 0)
        {
            FlashIcon(FlashAddFolder);
        }
    }

    // 清空队列/日志:Click 先于 Command 执行,故此时集合尚未清空,非空即代表"将要清掉" → 闪绿确认。
    private void OnClearQueueClick(object? sender, RoutedEventArgs e)
    {
        if (DataContext is MainWindowViewModel { Files.Count: > 0 })
        {
            FlashIcon(FlashClearQueue);
        }
    }

    private void OnClearLogsClick(object? sender, RoutedEventArgs e)
    {
        if (DataContext is MainWindowViewModel { Logs.Count: > 0 })
        {
            FlashIcon(FlashClearLog);
        }
    }

    // 主题/语言切换必然成功,无条件闪。主题图标 sun/moon 互换,两份绿副本都点亮,
    // Command 翻转后当前可见的那个绿副本泛起(另一个被 IsVisible 折叠,不显示)。
    private void OnToggleThemeClick(object? sender, RoutedEventArgs e)
    {
        FlashIcon(FlashThemeMoon);
        FlashIcon(FlashThemeSun);
    }

    private void OnToggleLanguageClick(object? sender, RoutedEventArgs e) => FlashIcon(FlashGlobe);

    // 成功微反馈:叠在原图标上的绿色副本短暂泛起再淡灭(Opacity transition 在 XAML)。
    private static void FlashIcon(Control icon)
    {
        icon.Opacity = 1;
        DispatcherTimer.RunOnce(() => icon.Opacity = 0, TimeSpan.FromSeconds(0.55));
    }

    private async void OnLogTapped(object? sender, TappedEventArgs e) => await CopySelectedAsync();

    private async void OnLogKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            LogList.SelectedIndex = -1;
            return;
        }

        // ⌘C(macOS)或 Ctrl+C 复制选中条
        if (e.Key == Key.C &&
            (e.KeyModifiers.HasFlag(KeyModifiers.Meta) || e.KeyModifiers.HasFlag(KeyModifiers.Control)))
        {
            await CopySelectedAsync();
        }
    }

    private async void OnCopyAllLogs(object? sender, RoutedEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            // 显示是新→旧;复制全部按时间顺序(旧→新)更易读。
            await SetClipboardAsync(string.Join("\n", vm.Logs.Reverse().Select(LogText)));
            ShowCopyHint();
        }
    }

    private async Task CopySelectedAsync()
    {
        if (LogList.SelectedItem is not LogLine line)
        {
            return;
        }

        var text = LogText(line);
        // 延后到所有 UI 事件(含 pointer 链)之后清除选中,确保高亮一定消失
        Dispatcher.UIThread.Post(() => LogList.SelectedIndex = -1, DispatcherPriority.Background);
        await SetClipboardAsync(text);
        ShowCopyHint();
    }

    private static string LogText(LogLine line) =>
        line.HasDetail ? $"{line.Time}  {line.Title} — {line.Detail}" : $"{line.Time}  {line.Title}";

    private void ShowCopyHint()
    {
        CopyHint.Opacity = 1;
        DispatcherTimer.RunOnce(() => CopyHint.Opacity = 0, TimeSpan.FromSeconds(1.0));
    }

    private async Task SetClipboardAsync(string text)
    {
        var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
        if (clipboard is not null)
        {
            await clipboard.SetTextAsync(text);
        }
    }
}
