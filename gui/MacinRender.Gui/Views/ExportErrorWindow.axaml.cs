using System;
using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using MacinRender.Gui.I18n;

namespace MacinRender.Gui.Views;

public partial class ExportErrorWindow : Window
{
    private readonly TextBlock? _copyHint;
    private readonly TextBox? _detailsBox;
    private readonly TextBlock? _subtitleBlock;
    private readonly string? _subtitleOverride;
    private readonly TextBlock? _titleBlock;
    private readonly string? _titleOverride;

    public ExportErrorWindow()
        : this("")
    {
    }

    public ExportErrorWindow(string details, string? title = null, string? subtitle = null)
    {
        _titleOverride = title;
        _subtitleOverride = subtitle;
        InitializeComponent();
        _detailsBox = this.FindControl<TextBox>("DetailsBox");
        _copyHint = this.FindControl<TextBlock>("CopyHint");
        _titleBlock = this.FindControl<TextBlock>("TitleBlock");
        _subtitleBlock = this.FindControl<TextBlock>("SubtitleBlock");
        if (_detailsBox is not null)
        {
            _detailsBox.Text = details;
        }

        UpdateTitle();
        Localizer.Instance.PropertyChanged += OnLocalizerChanged;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnLocalizerChanged(object? sender, PropertyChangedEventArgs e) => UpdateTitle();

    private void UpdateTitle()
    {
        var title = string.IsNullOrWhiteSpace(_titleOverride) ? Localizer.Instance["SemExportErrorTitle"] : _titleOverride;
        var subtitle = string.IsNullOrWhiteSpace(_subtitleOverride)
            ? Localizer.Instance["SemExportErrorSubtitle"]
            : _subtitleOverride;
        Title = title;
        if (_titleBlock is not null)
        {
            _titleBlock.Text = title;
        }
        if (_subtitleBlock is not null)
        {
            _subtitleBlock.Text = subtitle;
        }
    }

    private async void OnCopy(object? sender, RoutedEventArgs e)
    {
        var text = _detailsBox?.Text;
        var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
        if (!string.IsNullOrEmpty(text) && clipboard is not null)
        {
            await clipboard.SetTextAsync(text);
            ShowCopyHint();
        }
    }

    private void OnClose(object? sender, RoutedEventArgs e) => Close();

    private void ShowCopyHint()
    {
        if (_copyHint is null)
        {
            return;
        }

        _copyHint.Opacity = 1;
        DispatcherTimer.RunOnce(() => _copyHint.Opacity = 0, TimeSpan.FromSeconds(1.0));
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);
        Localizer.Instance.PropertyChanged -= OnLocalizerChanged;
    }
}
