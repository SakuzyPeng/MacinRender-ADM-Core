using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Styling;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MacinRender.Gui.Models;

namespace MacinRender.Gui.ViewModels;

/// <summary>
/// 花瓶 MVP 的 ViewModel:mock 数据 + 模拟进度,不接 C ABI、不做真实渲染。
/// 输出设置走真实约束模型(OutputModel),四级联动 + 不兼容禁用。
/// </summary>
public partial class MainWindowViewModel : ObservableObject
{
    public ObservableCollection<RenderFileItem> Files { get; } = new();
    public ObservableCollection<LogLine> Logs { get; } = new();

    // ── 输出设置:渲染(后端→布局)+ 封装(编码器→容器→位深/码率) ──
    public ObservableCollection<BackendDef> Backends => OutputModel.Backends;
    public ObservableCollection<LayoutDef> Layouts { get; } = new();
    public ObservableCollection<CodecOption> Codecs { get; } = new();
    public ObservableCollection<ContainerDef> Containers { get; } = new();

    [ObservableProperty] private BackendDef _selectedBackend = OutputModel.BackendById["ear"];
    [ObservableProperty] private LayoutDef? _selectedLayout;
    [ObservableProperty] private CodecOption? _selectedCodec;
    [ObservableProperty] private ContainerDef? _selectedContainer;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CodecColumnSpan))]
    private bool _showBitrate;
    [ObservableProperty] private bool _showFixedNote;
    [ObservableProperty] private string _fixedNote = "";
    [ObservableProperty] private string _bitrateLabel = "";

    /// <summary>编码器下拉占列数:有损=1(让出半列给码率),无损=2(占满左半区)。</summary>
    public int CodecColumnSpan => ShowBitrate ? 1 : 2;
    [ObservableProperty] private decimal _bitrate;
    [ObservableProperty] private decimal _bitrateMin;
    [ObservableProperty] private decimal _bitrateMax = 320;

    [ObservableProperty] private double _overallProgress;
    [ObservableProperty] private string _statusText = "就绪";
    [ObservableProperty] private bool _isDark = true; // 默认深色,驱动切换主题按钮的月亮/太阳图标

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LanguageText))]
    private bool _isEnglish; // 花瓶 mock:仅切换语言按钮自身文字

    public string LanguageText => IsEnglish ? "English" : "中文";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ActionButtonText))]
    [NotifyCanExecuteChangedFor(nameof(AddFileCommand))]
    [NotifyCanExecuteChangedFor(nameof(ClearQueueCommand))]
    private bool _isRendering;

    public string ActionButtonText => IsRendering ? "取消" : "开始渲染";

    private bool CanEditQueue => !IsRendering;

    private CancellationTokenSource? _cts;
    private int _counter = 3;
    private SubOption _lastSub = SubOption.None;

    public MainWindowViewModel()
    {
        Files.Add(new RenderFileItem { Name = "intro_atmos.wav" });
        Files.Add(new RenderFileItem { Name = "scene_02_objects.wav" });
        Files.Add(new RenderFileItem { Name = "finale_bed+obj.wav" });
        StatusText = $"就绪 — 队列中 {Files.Count} 个文件";
        AddLog(LogKind.Info, "已加载文件", $"{Files.Count} 个 ADM BWF");
        AddLog(LogKind.Info, "默认设置", "EAR · 7.1.4");

        RebuildLayouts(); // 触发四级联动初始化
    }

    // ── 联动 ──

    partial void OnSelectedBackendChanged(BackendDef value) => RebuildLayouts();
    partial void OnSelectedLayoutChanged(LayoutDef? value) => RebuildCodecs();
    partial void OnSelectedCodecChanged(CodecOption? value)
    {
        RebuildContainers();
        UpdateSubOptions();
    }

    private void RebuildLayouts()
    {
        var keep = SelectedLayout;
        Layouts.Clear();
        foreach (var id in SelectedBackend.LayoutIds)
        {
            Layouts.Add(OutputModel.LayoutById[id]);
        }

        var next = keep is not null && Layouts.Contains(keep) ? keep : Layouts.FirstOrDefault();
        if (!ReferenceEquals(next, SelectedLayout))
        {
            SelectedLayout = next; // → RebuildCodecs
        }
        else
        {
            RebuildCodecs();
        }
    }

    private void RebuildCodecs()
    {
        var layout = SelectedLayout;
        var keepId = SelectedCodec?.Def.Id;
        Codecs.Clear();
        foreach (var def in OutputModel.Codecs)
        {
            bool layoutOk;
            string? layoutReason = null;
            if (def.AllowedLayoutIds is { } whitelist)
            {
                layoutOk = layout is null || whitelist.Contains(layout.Id);
                if (!layoutOk)
                {
                    layoutReason = "不支持该布局";
                }
            }
            else
            {
                bool fitsChannels = layout is null || layout.Channels <= def.MaxChannels;
                bool fitsHeight = layout is null || def.SupportsHeight || !layout.HasHeight;
                layoutOk = fitsChannels && fitsHeight;
                if (!fitsChannels)
                {
                    layoutReason = $"超 {def.MaxChannels}ch";
                }
                else if (!fitsHeight)
                {
                    layoutReason = "不支持高度";
                }
            }

            bool enabled = def.Available && layoutOk;
            string label = def.Name;
            if (!def.Available)
            {
                label = $"{def.Name} — {def.Reason}";
            }
            else if (layoutReason is not null)
            {
                label = $"{def.Name} — {layoutReason}";
            }

            Codecs.Add(new CodecOption(def, enabled, label));
        }

        var next = Codecs.FirstOrDefault(c => c.Def.Id == keepId && c.IsEnabled)
                   ?? Codecs.FirstOrDefault(c => c.IsEnabled);
        if (!ReferenceEquals(next, SelectedCodec))
        {
            SelectedCodec = next; // → RebuildContainers + UpdateSubOptions
        }
        else
        {
            RebuildContainers();
            UpdateSubOptions();
        }
    }

    private void RebuildContainers()
    {
        var keepId = SelectedContainer?.Id;
        Containers.Clear();
        if (SelectedCodec is not null)
        {
            foreach (var cid in SelectedCodec.Def.ContainerIds)
            {
                Containers.Add(OutputModel.ContainerById[cid]);
            }
        }

        var next = Containers.FirstOrDefault(c => c.Id == keepId) ?? Containers.FirstOrDefault();
        if (!ReferenceEquals(next, SelectedContainer))
        {
            SelectedContainer = next;
        }
    }

    private void UpdateSubOptions()
    {
        var def = SelectedCodec?.Def;
        ShowBitrate = def?.Sub is SubOption.BitratePerCh or SubOption.BitrateTotal;
        ShowFixedNote = def?.Sub == SubOption.None;

        if (def is null)
        {
            return;
        }

        FixedNote = def.Id switch
        {
            "pcm" => "32-bit float",
            "flac" => "固定 24-bit",
            "iamf" => "Opus 内部编码",
            _ => ""
        };

        if (ShowBitrate)
        {
            decimal defaultKbps;
            if (def.Sub == SubOption.BitratePerCh)
            {
                BitrateMin = 6;
                BitrateMax = 320;
                BitrateLabel = "码率 kbps/声道";
                defaultKbps = 64; // Opus 默认 64 kbps/声道
            }
            else
            {
                BitrateMin = 64;
                BitrateMax = 12000;
                BitrateLabel = "码率 kbps 总";
                int ch = SelectedLayout?.Channels ?? 12;
                defaultKbps = Math.Round(2048m * ch / 12m); // APAC:7.1.4(12ch)=2048,按声道缩放
            }

            // 仅在编码类型切换、或当前值越界时回默认;手动改过的码率保留
            if (_lastSub != def.Sub || Bitrate < BitrateMin || Bitrate > BitrateMax)
            {
                Bitrate = defaultKbps;
            }
        }

        _lastSub = def.Sub;
    }

    // ── 队列 / 日志 / 渲染 ──

    [RelayCommand(CanExecute = nameof(CanEditQueue))]
    private void AddFile()
    {
        _counter++;
        Files.Add(new RenderFileItem { Name = $"clip_{_counter:00}.wav" });
        StatusText = $"就绪 — 队列中 {Files.Count} 个文件";
        AddLog(LogKind.Info, "添加文件", $"clip_{_counter:00}.wav");
    }

    [RelayCommand(CanExecute = nameof(CanEditQueue))]
    private void ClearQueue()
    {
        Files.Clear();
        OverallProgress = 0;
        StatusText = "队列已清空";
        AddLog(LogKind.Info, "清空队列");
    }

    [RelayCommand]
    private void ClearLogs() => Logs.Clear();

    [RelayCommand]
    private void ToggleLanguage() => IsEnglish = !IsEnglish;

    [RelayCommand]
    private void ToggleTheme()
    {
        if (Application.Current is { } app)
        {
            var next = app.ActualThemeVariant == ThemeVariant.Dark ? ThemeVariant.Light : ThemeVariant.Dark;
            app.RequestedThemeVariant = next;
            IsDark = next == ThemeVariant.Dark;
        }
    }

    [RelayCommand]
    private void Primary()
    {
        if (IsRendering)
        {
            _cts?.Cancel();
            return;
        }

        _ = RunRenderAsync();
    }

    private async Task RunRenderAsync()
    {
        if (IsRendering || Files.Count == 0)
        {
            return;
        }

        _cts = new CancellationTokenSource();
        var token = _cts.Token;
        IsRendering = true;
        OverallProgress = 0;
        Logs.Clear();

        var target = $"{SelectedLayout?.Name} · {SelectedContainer?.Name}";
        AddLog(LogKind.Info, "开始渲染", $"{Files.Count} 个文件 · {target}");

        foreach (var f in Files)
        {
            f.Progress = 0;
            f.Status = FileStatus.Pending;
        }

        try
        {
            for (int i = 0; i < Files.Count; i++)
            {
                var f = Files[i];
                f.Status = FileStatus.Rendering;
                StatusText = $"渲染中 ({i + 1}/{Files.Count}) — {f.Name}";
                AddLog(LogKind.Info, "正在渲染", $"{f.Name} → {SelectedLayout?.Name}");

                for (int p = 1; p <= 20; p++)
                {
                    await Task.Delay(70, token);
                    f.Progress = p * 5;
                    OverallProgress = (i + p / 20.0) / Files.Count * 100.0;
                }

                f.Status = FileStatus.Done;
                AddLog(LogKind.Success, "已完成", f.Name);
            }

            OverallProgress = 100;
            StatusText = $"全部完成 — {Files.Count} 个文件";
            AddLog(LogKind.Success, "全部完成", $"{Files.Count} 个文件");
        }
        catch (OperationCanceledException)
        {
            StatusText = "已取消";
            AddLog(LogKind.Warn, "已取消", "用户中断渲染");
            foreach (var f in Files)
            {
                if (f.Status == FileStatus.Rendering)
                {
                    f.Status = FileStatus.Pending;
                }
            }
        }
        finally
        {
            IsRendering = false;
            _cts?.Dispose();
            _cts = null;
        }
    }

    private void AddLog(LogKind kind, string title, string detail = "") =>
        Logs.Add(new LogLine(kind, title, detail, DateTime.Now));
}
