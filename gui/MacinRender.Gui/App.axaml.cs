using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;
using System;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Models;
using MacinRender.Gui.Services;
using MacinRender.Gui.ViewModels;
using MacinRender.Gui.Views;

namespace MacinRender.Gui;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        // 用真实 C ABI 查询填充输出模型(backends/layouts/formats);失败即中止启动。
        LoadOutputModel();

        // 建窗前先应用持久化的主题/语言,避免首帧深色→浅色 / 中→英闪烁(VM 只恢复输出设置)。
        ApplyPersistedThemeAndLanguage();

        // i18n:把当前语言字典灌入 App 资源(供 XAML {DynamicResource});切语言时刷新。
        Localizer.Instance.PropertyChanged += (_, _) => ApplyLanguageResources();
        ApplyLanguageResources();

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = new MainWindow
            {
                DataContext = new MainWindowViewModel()
            };
        }

        base.OnFrameworkInitializationCompleted();
    }

    // 在订阅/灌资源之前应用,确保首次就灌入正确语言;主题直接落到 Application 级,窗口随之以正确主题首绘。
    private void ApplyPersistedThemeAndLanguage()
    {
        var s = SettingsStore.Load();
        if (s is null)
        {
            return;
        }

        if (s.IsEnglish)
        {
            Localizer.Instance.SetLanguage(Lang.En);
        }

        if (!s.IsDark)
        {
            RequestedThemeVariant = ThemeVariant.Light;
        }
    }

    private void ApplyLanguageResources()
    {
        foreach (var key in Localizer.Instance.Keys)
        {
            Resources[key] = Localizer.Instance[key];
        }

    }

    private async void OnAboutMenuClick(object? sender, EventArgs e)
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime { MainWindow: { } mainWindow })
        {
            await new AboutWindow().ShowDialog(mainWindow);
        }
    }

    private static void LoadOutputModel()
    {
        try
        {
            using var ctx = NativeMethods.adm_create_context();
            if (ctx.IsInvalid)
            {
                throw new InvalidOperationException("adm_create_context returned NULL.");
            }

            var matrix = AdmQueries.LoadSupportMatrix(ctx)
                ?? throw new InvalidOperationException("adm_render_support_matrix_json returned no data.");
            OutputModel.Initialize(matrix);
            // 系统空间音频可选布局来自 capabilities 的 system_spatial_layouts(权威源,跨平台,非硬编码);
            // 不支持平台(如 Linux)为空。
            OutputModel.InitializeSystemSpatial(AdmQueries.LoadCapabilities(ctx));
            // 各布局逐声道标签来自 adm_layouts_json(CoreAudio 顺序),供多声道电平表标注。须在上一行之后。
            OutputModel.InitializeLayoutOrders(AdmQueries.LoadLayouts(ctx));
        }
        catch (DllNotFoundException ex)
        {
            throw new InvalidOperationException(
                "无法加载 libmradm_capi。请先构建 mradm_capi_bundle 并运行 gui/copy-native.sh。", ex);
        }
        catch (EntryPointNotFoundException ex)
        {
            throw new InvalidOperationException(
                "libmradm_capi 版本过旧或不匹配,缺少 adm_render_support_matrix_json。请重新构建并运行 gui/copy-native.sh。",
                ex);
        }
    }
}
