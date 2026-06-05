using System;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Input.Platform;
using Avalonia.Interactivity;
using Avalonia.Threading;
using MacinRender.Gui.Models;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private void OnTitleBarPressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            BeginMoveDrag(e);
        }
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
            await SetClipboardAsync(string.Join("\n", vm.Logs.Select(LogText)));
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
