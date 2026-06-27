using System;
using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using MacinRender.Gui.I18n;

namespace MacinRender.Gui.Views;

// 多声道(系统空间音频)逐声道电平表的独立窗口。DataContext 由唤出方(语义编辑视图)共享同一个
// VM,于是 ChannelMeters / LUFS 全部白送、随监听轮询实时联动(传统竖条表)。
public partial class ChannelMeterWindow : Window
{
    public ChannelMeterWindow()
    {
        InitializeComponent();
        UpdateTitle();
        Localizer.Instance.PropertyChanged += OnLocalizerChanged;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnLocalizerChanged(object? sender, PropertyChangedEventArgs e) => UpdateTitle();

    private void UpdateTitle() => Title = Localizer.Instance["SemMonChannelMeterTitle"];

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        Localizer.Instance.PropertyChanged -= OnLocalizerChanged;
    }
}
