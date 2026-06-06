using System;
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
        AddHandler(DragDrop.DropEvent, OnDrop);
    }

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        bool ok = e.DataTransfer.Contains(DataFormat.File) && DataContext is MainWindowViewModel { IsRendering: false };
        e.DragEffects = ok ? DragDropEffects.Copy : DragDropEffects.None;
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
        {
            return;
        }

        var paths = e.DataTransfer.TryGetFiles()?
            .Select(f => f.TryGetLocalPath())
            .Where(p => !string.IsNullOrEmpty(p) && IsAdmFile(p!))
            .Select(p => p!)
            .ToList();
        if (paths is { Count: > 0 })
        {
            vm.AddFiles(paths);
        }
    }

    private static bool IsAdmFile(string path) =>
        AdmExtensions.Contains(Path.GetExtension(path).ToLowerInvariant());

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
        vm.AddFiles(paths);
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
