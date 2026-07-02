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

    public ExportErrorWindow()
        : this("")
    {
    }

    public ExportErrorWindow(string details)
    {
        InitializeComponent();
        _detailsBox = this.FindControl<TextBox>("DetailsBox");
        _copyHint = this.FindControl<TextBlock>("CopyHint");
        if (_detailsBox is not null)
        {
            _detailsBox.Text = details;
        }

        UpdateTitle();
        Localizer.Instance.PropertyChanged += OnLocalizerChanged;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnLocalizerChanged(object? sender, PropertyChangedEventArgs e) => UpdateTitle();

    private void UpdateTitle() => Title = Localizer.Instance["SemExportErrorTitle"];

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
