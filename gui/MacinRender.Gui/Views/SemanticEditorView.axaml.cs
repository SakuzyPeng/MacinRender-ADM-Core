using System;
using System.Collections.Generic;
using System.IO;
using Avalonia.Controls;
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
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

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

    // 选监听用自定义 HRIR(SOFA)→ 交 VM(SAF 双耳后端时即时重载)。
    private async void OnPickMonitorSofa(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel vm)
        {
            return;
        }

        var path = await MainWindow.PickSofaAsync(this);
        if (!string.IsNullOrEmpty(path))
        {
            vm.SetMonitorSofa(path);
        }
    }

    // 顶栏 HRIR 两态按钮:未选择时选择 SOFA;已选择时清除,回到默认 HRTF。
    private async void OnToggleMonitorSofa(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel vm)
        {
            return;
        }

        if (vm.HasMonitorSofa)
        {
            vm.ClearMonitorSofaCommand.Execute(null);
            return;
        }

        var path = await MainWindow.PickSofaAsync(this);
        if (!string.IsNullOrEmpty(path))
        {
            vm.SetMonitorSofa(path);
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

    // 单击 extent 轴名 → 切换该轴是否联动(联动轴共享值)。Tag = "W"/"H"/"D"。
    private void OnToggleAxisLink(object? sender, TappedEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel { SelectedRow.Extent: { } ext } ||
            sender is not Control { Tag: string which })
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
