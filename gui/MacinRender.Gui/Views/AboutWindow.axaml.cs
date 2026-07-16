using System;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using MacinRender.Gui.Interop;

namespace MacinRender.Gui.Views;

public partial class AboutWindow : Window
{
    public AboutWindow()
    {
        InitializeComponent();
        DataContext = this;
    }

    public string ProductVersion => BuildInfo.ProductVersion;

    public string CApiVersion =>
        $"v{NativeMethods.adm_api_version_major()}.{NativeMethods.adm_api_version_minor()}.{NativeMethods.adm_api_version_patch()}";

    public string Commit => BuildInfo.Commit;

    public string ShortCommit => Commit.Length > 12 ? Commit[..12] : Commit;

    private void OnTitleBarPressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            BeginMoveDrag(e);
        }
    }

    private async void OnProjectPressed(object? sender, RoutedEventArgs e)
    {
        await Launcher.LaunchUriAsync(new Uri(BuildInfo.ProjectUrl));
    }

    private void OnClosePressed(object? sender, RoutedEventArgs e) => Close();
}
