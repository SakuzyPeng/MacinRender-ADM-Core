using System;
using System.Collections.Generic;
using System.IO;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using MacinRender.Gui.I18n;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui.Views;

public partial class SemanticEditorView : UserControl
{
    public SemanticEditorView()
    {
        InitializeComponent();
        // 进度条 Thumb 拖动起止:Thumb 的 DragStarted/DragCompleted 路由事件冒泡到 Slider 上接住。
        // 本视图用手写 InitializeComponent(AvaloniaXamlLoader.Load),命名字段不自动赋值 → 用 FindControl 取。
        var seek = this.FindControl<Slider>("SeekSlider");
        if (seek is not null)
        {
            seek.AddHandler(Thumb.DragStartedEvent, OnSeekDragStarted);
            seek.AddHandler(Thumb.DragCompletedEvent, OnSeekDragCompleted);
        }

        // 长按计时器(手动实现长按:鼠标按下达阈值即切换该轴联动,见 OnAxisPointerPressed)。
        _holdTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _holdTimer.Tick += (_, _) =>
        {
            _holdTimer.Stop();
            ToggleAxisLink(_holdAxis);
            _holdAxis = null;
        };
    }

    // ── extent 轴名长按切换联动 ──
    // 改长按(而非单击)是为了和"双击重置"彻底分开:快速双击的两次按下都在 500ms 阈值前 release,
    // 计时器被取消 → 不会误触联动切换;只有真正按住不放达阈值才切换。
    private readonly DispatcherTimer _holdTimer;
    private string? _holdAxis;

    private void OnAxisPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (sender is Control { Tag: string which })
        {
            _holdAxis = which;
            _holdTimer.Stop();
            _holdTimer.Start();
        }
    }

    private void OnAxisPointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        _holdTimer.Stop();
        _holdAxis = null;
    }

    // 切换某轴联动(Tag = "W"/"H"/"D")。联动轴共享值;未联动轴独立。
    private void ToggleAxisLink(string? which)
    {
        if (which is null || DataContext is not SemanticEditorViewModel { SelectedRow.Extent: { } ext })
        {
            return;
        }

        switch (which)
        {
            case "W":
                ext.WidthLinked = !ext.WidthLinked;
                break;
            case "H":
                ext.HeightLinked = !ext.HeightLinked;
                break;
            case "D":
                ext.DepthLinked = !ext.DepthLinked;
                break;
        }
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    // 开始拖动 thumb:进入 scrub(引擎照旧从原位置播,不被刷屏 seek 打断)。
    private void OnSeekDragStarted(object? sender, VectorEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel vm)
        {
            vm.BeginScrub();
        }
    }

    // 松开 thumb:退出 scrub 并做一次干净 seek 跳到目标位置。
    private void OnSeekDragCompleted(object? sender, VectorEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel vm)
        {
            vm.EndScrub();
        }
    }

    // 文件选择走视图 code-behind(同批渲染界面),拿到本地路径后交 VM 载入 + inspect。
    private async void OnLoadFile(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel vm)
        {
            return;
        }

        var top = TopLevel.GetTopLevel(this);
        if (top is null)
        {
            return;
        }

        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = Localizer.Instance["PickTitle"],
            AllowMultiple = false,
            FileTypeFilter = new List<FilePickerFileType>
            {
                new(Localizer.Instance["PickFilter"]) { Patterns = new[] { "*.wav" } },
            },
        });

        if (files.Count == 0)
        {
            return;
        }

        var path = files[0].TryGetLocalPath();
        if (!string.IsNullOrEmpty(path) && await vm.LoadFileAsync(path))
        {
            FlashIcon(FlashLoadFile);
        }
    }

    // 导出生效 ADM:选好保存路径后交 VM 写回(adm_export_file,复用源 PCM)。
    private async void OnExport(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel { CanExport: true } vm)
        {
            return;
        }

        var top = TopLevel.GetTopLevel(this);
        if (top is null)
        {
            return;
        }

        var suggested = string.IsNullOrEmpty(vm.LoadedPath)
            ? "effective.wav"
            : Path.GetFileNameWithoutExtension(vm.LoadedPath) + ".effective.wav";
        var file = await top.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = Localizer.Instance["SemExport"],
            SuggestedFileName = suggested,
            DefaultExtension = "wav",
            FileTypeChoices = new List<FilePickerFileType>
            {
                new("ADM BWF") { Patterns = new[] { "*.wav" } },
            },
        });

        var path = file?.TryGetLocalPath();
        if (!string.IsNullOrEmpty(path) && await vm.ExportToAsync(path))
        {
            FlashIcon(FlashExport);
        }
    }

    // 成功微反馈:叠在原图标上的绿色副本短暂泛起再淡灭(Opacity transition 在 XAML)。与批渲染同一套。
    private static void FlashIcon(Control icon)
    {
        icon.Opacity = 1;
        DispatcherTimer.RunOnce(() => icon.Opacity = 0, TimeSpan.FromSeconds(0.55));
    }

    // 浏览监听用自定义 HRIR(SOFA)→ 加入 MRU 并选中(SAF 双耳后端时即时重载)。成功闪绿。
    private async void OnPickMonitorSofa(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel vm)
        {
            return;
        }

        var path = await MainWindow.PickSofaAsync(this);
        if (!string.IsNullOrEmpty(path))
        {
            vm.MonitorSofa.Pick(path);
            var flash = this.FindControl<Control>("FlashMonitorSofa");
            if (flash is not null)
            {
                FlashIcon(flash);
            }
        }
    }

    // 双击「对象」标题 → 清空全部对象覆盖。
    private void OnResetAll(object? sender, TappedEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel vm)
        {
            vm.ResetAllOverrides();
        }
    }

    // 双击对象名(编辑器标题)→ 清空当前对象覆盖。
    private void OnResetRow(object? sender, TappedEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel { SelectedRow: { } row })
        {
            row.ResetAll();
        }
    }

    // 双击维度名(gain/diffuse/extent/divergence)→ 清空该维度覆盖。
    private void OnResetDim(object? sender, TappedEventArgs e)
    {
        if (sender is Control { Tag: IResettableOverride ov })
        {
            ov.Reset();
        }
    }

    // 双击 extent 轴名 → 重置整个 extent(三轴回中性 + 联动复位为默认)。extent 是单一听感维度,
    // 三轴是其分面 → 与其它维度"双击维度名即重置"一致。长按切换联动与双击互不干扰(见 OnAxisPointerPressed)。
    private void OnResetExtent(object? sender, TappedEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel { SelectedRow.Extent: { } ext })
        {
            ext.Reset();
        }
    }

    // 回车提交数值输入框:把焦点移回视图,触发 TextBox 失焦 → 绑定回写并规范化文本。
    private void OnValueEntryKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            Focus();
            e.Handled = true;
        }
    }
}
