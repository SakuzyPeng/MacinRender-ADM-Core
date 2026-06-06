using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using System;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Models;
using MacinRender.Gui.ViewModels;

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

    private void ApplyLanguageResources()
    {
        foreach (var key in Localizer.Instance.Keys)
        {
            Resources[key] = Localizer.Instance[key];
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
