using Avalonia;
using System;

namespace MacinRender.Gui;

class Program
{
    // Initialization code. Don't use any Avalonia, third-party APIs or any
    // SynchronizationContext-reliant code before AppMain is called: things aren't initialized
    // yet and stuff might break.
    [STAThread]
    public static int Main(string[] args)
    {
        // headless 桥接层自检,不进 UI(详见 SelfTest)。
        if (args.Length > 0 && args[0] == "--selftest")
        {
            return SelfTest.Run(args.Length > 1 ? args[1] : null).GetAwaiter().GetResult();
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    // Avalonia configuration, don't remove; also used by visual designer.
    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
#if DEBUG
            .WithDeveloperTools()
#endif
            // 去掉 Inter(web 字体味),改用 macOS 系统字体 SF Pro
            .LogToTrace();
}
