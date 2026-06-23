using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Platform.Storage;
using MacinRender.Gui.I18n;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui.Views;

// 对象空间视图的独立窗口。DataContext 由唤出方(语义编辑视图)直接共享同一个 VM,
// 于是播放头(PlayheadSeconds)、空间模型(SpatialModel)、传输控制全部白送、实时联动。
public partial class SpatialWindow : Window
{
    public SpatialWindow()
    {
        InitializeComponent();
        UpdateTitle();
        Localizer.Instance.PropertyChanged += OnLocalizerChanged;

        // scrub:与主视图一致,Thumb 拖动起止 → BeginScrub/EndScrub(拖动中只刷新画面,松手才 seek)。
        var seek = this.FindControl<Slider>("SeekSlider");
        if (seek is not null)
        {
            seek.AddHandler(Thumb.DragStartedEvent, OnSeekDragStarted);
            seek.AddHandler(Thumb.DragCompletedEvent, OnSeekDragCompleted);
        }

        // 拖入 64×64 PNG 皮肤即换装(类 SOFA:直接拖、记忆上次)。
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
        AddHandler(DragDrop.DropEvent, OnDrop);
    }

    private static bool IsPng(string path) =>
        string.Equals(Path.GetExtension(path), ".png", StringComparison.OrdinalIgnoreCase);

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        bool ok = e.DataTransfer.Contains(DataFormat.File) &&
            (e.DataTransfer.TryGetFiles()?.Select(f => f.TryGetLocalPath())
                .Any(p => !string.IsNullOrEmpty(p) && IsPng(p!)) ?? false);
        e.DragEffects = ok ? DragDropEffects.Copy : DragDropEffects.None;
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
        if (DataContext is not SemanticEditorViewModel vm)
        {
            return;
        }

        var png = e.DataTransfer.TryGetFiles()?
            .Select(f => f.TryGetLocalPath())
            .FirstOrDefault(p => !string.IsNullOrEmpty(p) && IsPng(p!));
        if (!string.IsNullOrEmpty(png))
        {
            vm.LoadSkin(png!);
        }
    }

    private void OnLocalizerChanged(object? sender, PropertyChangedEventArgs e) => UpdateTitle();

    private void UpdateTitle() => Title = Localizer.Instance["SemSpatialTitle"];

    private void OnSeekDragStarted(object? sender, VectorEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel vm)
        {
            vm.BeginScrub();
        }
    }

    private void OnSeekDragCompleted(object? sender, VectorEventArgs e)
    {
        if (DataContext is SemanticEditorViewModel vm)
        {
            vm.EndScrub();
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        Localizer.Instance.PropertyChanged -= OnLocalizerChanged;
    }
}
