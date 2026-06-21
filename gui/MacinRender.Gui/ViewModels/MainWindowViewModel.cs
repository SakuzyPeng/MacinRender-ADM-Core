using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Styling;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Models;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.ViewModels;

/// <summary>
/// 渲染界面 ViewModel:输出设置由真实支持矩阵(OutputModel)驱动,
/// 文件队列经稳定 C ABI 调 AdmRenderService 执行离线渲染。
/// </summary>
public partial class MainWindowViewModel : ObservableObject
{
    public ObservableCollection<RenderFileItem> Files { get; } = new();
    public ObservableCollection<LogLine> Logs { get; } = new();

    // ── 模式导航:批渲染 / 语义编辑(共用 Col 1 主区,按 IsBatchMode/IsSemanticMode 切换) ──
    public SemanticEditorViewModel SemanticEditor { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsBatchMode))]
    [NotifyPropertyChangedFor(nameof(IsSemanticMode))]
    private bool _semanticMode;

    public bool IsBatchMode => !SemanticMode;
    public bool IsSemanticMode => SemanticMode;

    [RelayCommand]
    private void ShowBatchMode() => SemanticMode = false;

    [RelayCommand]
    private void ShowSemanticMode() => SemanticMode = true;

    // ── 输出设置:渲染(后端→布局)+ 封装(编码器→容器→位深/码率) ──
    public ObservableCollection<BackendDef> Backends => OutputModel.Backends;
    public ObservableCollection<LayoutDef> Layouts { get; } = new();
    public ObservableCollection<CodecOption> Codecs { get; } = new();
    public ObservableCollection<ContainerDef> Containers { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SofaApplicable))]
    private BackendDef _selectedBackend = OutputModel.BackendById["ear"];
    [ObservableProperty] private LayoutDef? _selectedLayout;
    [ObservableProperty] private CodecOption? _selectedCodec;
    [ObservableProperty] private ContainerDef? _selectedContainer;

    // 自定义 HRIR(SOFA):只对 SAF 双耳后端(binaural / saf-binaural)有效;Apple 双耳用自家 HRTF。
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasSofa))]
    [NotifyPropertyChangedFor(nameof(SofaFileName))]
    private string? _sofaPath;

    public bool SofaApplicable =>
        OutputModel.SofaAvailable && SelectedBackend.Renderer is AdmRenderer.Binaural or AdmRenderer.SafBinaural;
    public bool HasSofa => !string.IsNullOrEmpty(SofaPath);
    public string SofaFileName => HasSofa ? Path.GetFileName(SofaPath!) : "";

    public void SetSofa(string path) => SofaPath = path;

    [RelayCommand]
    private void ClearSofa() => SofaPath = null;

    partial void OnSofaPathChanged(string? value) => SaveSettings();

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
    private bool _isEnglish; // 当前 UI 语言;按钮显示目标语言

    // 语言按钮显示「目标」语言(点了会切到的那个):中文界面显示 English,反之显示 中文。
    public string LanguageText => IsEnglish ? "中文" : "English";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ActionButtonText))]
    [NotifyCanExecuteChangedFor(nameof(ClearQueueCommand))]
    private bool _isRendering;

    public string ActionButtonText => L[IsRendering ? "Cancel" : "StartRender"];

    private bool CanEditQueue => !IsRendering;

    private static Localizer L => Localizer.Instance;
    private readonly AdmRenderService _renderService = new();
    private CancellationTokenSource? _cts;
    private SubOption _lastSub = SubOption.None;
    private decimal _lastAutoBitrate; // 上次自动设的码率;与当前相等即视为"用户未手动改",可随布局重算
    private string _statusKey = "StatusReadyEmpty"; // 状态栏当前 key + 参数,切语言时按此重算
    private object?[] _statusArgs = Array.Empty<object?>();
    private bool _settingsReady; // 启动恢复完成前不持久化,避免把默认/中间联动态写盘

    public MainWindowViewModel()
    {
        // 主题/语言已由 App 在建窗前应用;同步本地状态(驱动主题按钮图标 + 语言按钮文案),不反向触发切换。
        IsDark = Application.Current?.ActualThemeVariant != ThemeVariant.Light;
        IsEnglish = Localizer.Instance.Current == Lang.En;

        Localizer.Instance.PropertyChanged += (_, _) => OnLanguageChanged();
        SetStatus("StatusReadyEmpty");
        AddLog(LogKind.Info, "LogReady", "LogReadyHint");

        RebuildCodecs(); // 触发联动初始化(后端→编码器→布局→容器)

        RestoreSettings(); // 应用上次保存的选择(在联动初始化之后,按链恢复)
        _settingsReady = true; // 此后任何用户改动都持久化
    }

    // 状态栏文案集中设置:记录 key+参数,切语言时重算(StatusText 含动态数据,故用 Format)。
    private void SetStatus(string key, params object?[] args)
    {
        _statusKey = key;
        _statusArgs = args;
        StatusText = L.Format(key, args);
    }

    // 语言切换时刷新非 DynamicResource 绑定的 VM 文案(计算属性 + 状态栏 + 码率标签)。
    private void OnLanguageChanged()
    {
        OnPropertyChanged(nameof(ActionButtonText));
        SetStatus(_statusKey, _statusArgs);
        UpdateSubOptions();
        foreach (var log in Logs)
        {
            log.RefreshLanguage(); // 历史日志也随语言切换刷新 Title/Detail
        }

        foreach (var file in Files)
        {
            file.RefreshLanguage(); // 正在渲染项的阶段文本也刷新
        }
    }

    // ── 联动 ──

    // 单向联动链:后端 → 编码器 → 布局 → 容器。切下游永远不会回改上游已选项。
    // 每个用户可改维度的变化都触发持久化(_settingsReady 前的初始化/恢复态被 SaveSettings 内部忽略)。
    partial void OnSelectedBackendChanged(BackendDef value)
    {
        RebuildCodecs();
        SaveSettings();
    }

    partial void OnSelectedCodecChanged(CodecOption? value)
    {
        RebuildLayouts();
        SaveSettings();
    }

    partial void OnSelectedLayoutChanged(LayoutDef? value)
    {
        RebuildContainers();
        UpdateSubOptions();
        SaveSettings();
    }

    partial void OnSelectedContainerChanged(ContainerDef? value) => SaveSettings();
    partial void OnBitrateChanged(decimal value) => SaveSettings();
    partial void OnIsDarkChanged(bool value) => SaveSettings();
    partial void OnIsEnglishChanged(bool value) => SaveSettings();

    // 编码器列表 = 当前后端在「任一支持布局」下可用的编码器(apac 在非 macOS 经 support-matrix 自动排除)。
    private void RebuildCodecs()
    {
        var keepId = SelectedCodec?.Def.Id;
        Codecs.Clear();
        foreach (var def in OutputModel.Codecs)
        {
            if (OutputModel.IsCodecSupportedByBackend(SelectedBackend, def.Id))
            {
                Codecs.Add(new CodecOption(def, true, def.Name));
            }
        }

        var next = Codecs.FirstOrDefault(c => c.Def.Id == keepId) ?? Codecs.FirstOrDefault();
        if (!ReferenceEquals(next, SelectedCodec))
        {
            SelectedCodec = next; // → RebuildLayouts
        }
        else
        {
            RebuildLayouts();
        }
    }

    // 布局列表 = 当前后端 ∩ 当前编码器支持的布局 —— 不兼容的布局不出现,
    // 故切布局永远不会反过来改掉已选编码器。
    private void RebuildLayouts()
    {
        var keepId = SelectedLayout?.Id;
        Layouts.Clear();
        if (SelectedCodec is not null)
        {
            foreach (var id in SelectedBackend.LayoutIds)
            {
                if (OutputModel.IsCodecSupported(SelectedBackend.Id, id, SelectedCodec.Def.Id))
                {
                    Layouts.Add(OutputModel.LayoutById[id]);
                }
            }
        }

        var next = (keepId is not null ? Layouts.FirstOrDefault(l => l.Id == keepId) : null)
                   ?? Layouts.FirstOrDefault();
        if (!ReferenceEquals(next, SelectedLayout))
        {
            SelectedLayout = next; // → RebuildContainers + UpdateSubOptions
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
        if (SelectedCodec is not null && SelectedLayout is not null)
        {
            foreach (var tid in OutputModel.SupportedContainers(SelectedBackend.Id, SelectedLayout.Id,
                         SelectedCodec.Def.Id))
            {
                Containers.Add(OutputModel.ContainerById[tid]);
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
            "flac" => L["FixedNoteFlac"],
            _ => ""
        };

        if (ShowBitrate)
        {
            decimal defaultKbps;
            if (def.Sub == SubOption.BitratePerCh)
            {
                BitrateMin = 6;
                BitrateMax = 320;
                BitrateLabel = L["BitratePerCh"];
                defaultKbps = 64; // Opus 默认 64 kbps/声道
            }
            else
            {
                BitrateMin = 64;
                BitrateMax = 12000;
                BitrateLabel = L["BitrateTotal"];
                int ch = SelectedLayout?.Channels ?? 12;
                defaultKbps = Math.Round(2048m * ch / 12m); // APAC:7.1.4(12ch)=2048,按声道缩放
            }

            // 回默认的条件:切编码类型 / 越界 / 当前仍是上次自动设的值(用户没手动改过)。
            // 末项让 APAC 默认(随声道数变)在切换布局时自动跟随;一旦用户手动改过则保留。
            bool stillAuto = Bitrate == _lastAutoBitrate;
            if (_lastSub != def.Sub || Bitrate < BitrateMin || Bitrate > BitrateMax || stillAuto)
            {
                Bitrate = defaultKbps;
                _lastAutoBitrate = defaultKbps;
            }
        }

        _lastSub = def.Sub;
    }

    // ── 设置持久化 ──

    // 启动时恢复输出设置(主题/语言已由 App 建窗前应用)。按联动链顺序(后端→编码器→布局→容器→码率)恢复,
    // 每步在当前支持矩阵里匹配 id,失配则保持默认(矩阵随平台/版本变,旧值可能已不可用)。
    private void RestoreSettings()
    {
        var s = SettingsStore.Load();
        if (s is null)
        {
            return;
        }

        if (s.Backend is not null && OutputModel.BackendById.TryGetValue(s.Backend, out var backend)
            && !ReferenceEquals(backend, SelectedBackend))
        {
            SelectedBackend = backend; // → 重建下游
        }

        if (s.Codec is not null && Codecs.FirstOrDefault(c => c.Def.Id == s.Codec) is { } codec
            && !ReferenceEquals(codec, SelectedCodec))
        {
            SelectedCodec = codec;
        }

        if (s.Layout is not null && Layouts.FirstOrDefault(l => l.Id == s.Layout) is { } layout
            && !ReferenceEquals(layout, SelectedLayout))
        {
            SelectedLayout = layout;
        }

        if (s.Container is not null && Containers.FirstOrDefault(c => c.Id == s.Container) is { } container
            && !ReferenceEquals(container, SelectedContainer))
        {
            SelectedContainer = container;
        }

        // 码率最后恢复:SelectedLayout 变化时 UpdateSubOptions 已把 Bitrate 重算成默认,这里覆盖回保存值。
        // 置 _lastAutoBitrate 为不等值 → 视作"用户手动码率",后续切布局不再自动覆盖。
        if (s.Bitrate is { } br && ShowBitrate && br >= BitrateMin && br <= BitrateMax)
        {
            Bitrate = br;
            _lastAutoBitrate = -1m;
        }

        if (!string.IsNullOrEmpty(s.SofaPath))
        {
            SofaPath = s.SofaPath;
        }
    }

    private void SaveSettings()
    {
        if (!_settingsReady)
        {
            return;
        }

        SettingsStore.Save(new AppSettings
        {
            Backend = SelectedBackend.Id,
            Codec = SelectedCodec?.Def.Id,
            Layout = SelectedLayout?.Id,
            Container = SelectedContainer?.Id,
            Bitrate = ShowBitrate ? Bitrate : null,
            IsDark = IsDark,
            IsEnglish = IsEnglish,
            SofaPath = SofaPath,
            MonitorSofaPath = SemanticEditor.MonitorSofaPath,
        });
    }

    // ── 队列 / 日志 / 渲染 ──

    /// <summary>
    /// 由拖拽 / 文件选择器(code-behind)调用。扩展名只是初筛,这里用 probe 做内容级 ADM 判定
    /// (chna+axml):普通 wav / 非 ADM 文件被拒绝入队,只记一条"跳过 N 个"提示。再按路径去重入队。
    /// </summary>
    public async Task<int> AddFilesAsync(IReadOnlyList<string> paths)
    {
        if (IsRendering || paths.Count == 0)
        {
            return 0;
        }

        var admPaths = await _renderService.FilterAdmAsync(paths);
        int skipped = paths.Count - admPaths.Count;

        var added = new List<string>();
        foreach (var p in admPaths)
        {
            if (string.IsNullOrEmpty(p) || Files.Any(f => f.InputPath == p))
            {
                continue;
            }

            Files.Add(new RenderFileItem { Name = Path.GetFileName(p), InputPath = p });
            added.Add(Path.GetFileName(p));
        }

        if (added.Count > 0)
        {
            SetStatus("StatusReadyN", Files.Count);
            AddLog(LogKind.Info, "LogAddFile", added.Count == 1 ? "Raw" : "NFiles",
                added.Count == 1 ? added[0] : added.Count);
        }

        if (skipped > 0)
        {
            AddLog(LogKind.Warn, "LogSkipNonAdm", "NFiles", skipped);
        }

        return added.Count;
    }

    [RelayCommand(CanExecute = nameof(CanEditQueue))]
    private void ClearQueue()
    {
        Files.Clear();
        OverallProgress = 0;
        SetStatus("StatusQueueCleared");
        AddLog(LogKind.Info, "TipClearQueue");
    }

    [RelayCommand]
    private void ClearLogs() => Logs.Clear();

    [RelayCommand]
    private void ToggleLanguage()
    {
        var next = Localizer.Instance.Current == Lang.En ? Lang.Zh : Lang.En;
        Localizer.Instance.SetLanguage(next);
        IsEnglish = next == Lang.En;
    }

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
        if (IsRendering)
        {
            return;
        }

        if (Files.Count == 0)
        {
            SetStatus("StatusQueueEmpty");
            AddLog(LogKind.Warn, "LogNoFile", "LogNoFileHint");
            return;
        }

        _cts = new CancellationTokenSource();
        var token = _cts.Token;
        IsRendering = true;
        OverallProgress = 0;

        var settings = BuildSettings();
        AddLog(LogKind.Info, "LogStart", "Raw",
            $"{Files.Count} × {SelectedBackend.Name} · {SelectedLayout?.Name} · {SelectedContainer?.Name}");

        // 渲染开始时的队列快照(遍历期间会增删 Files,故用快照驱动而非索引)。
        var queue = Files.ToList();
        foreach (var f in queue)
        {
            f.Progress = 0;
            f.Status = FileStatus.Pending;
        }

        int total = queue.Count;
        int done = 0;
        int processed = 0;
        bool cancelled = false;
        try
        {
            foreach (var f in queue)
            {
                if (token.IsCancellationRequested)
                {
                    cancelled = true;
                    break;
                }

                int idx = processed;
                f.Progress = 0;
                f.Status = FileStatus.Rendering;
                SetStatus("StatusRendering", processed + 1, total, f.Name);

                var progress = new Progress<RenderProgress>(p =>
                {
                    f.Progress = p.OverallFraction * 100.0;
                    f.StageKey = StageWordKey(p);
                    OverallProgress = (idx + p.OverallFraction) / total * 100.0;
                });

                var outPath = DeriveOutputPath(f.InputPath);
                var outcome = await _renderService.RenderAsync(f.InputPath, outPath, settings, progress, token);
                processed++;

                if (token.IsCancellationRequested || outcome.ErrorCode == AdmErrorCode.Cancelled)
                {
                    f.Status = FileStatus.Pending;
                    cancelled = true;
                    break;
                }

                foreach (var log in outcome.Logs)
                {
                    if (log.Level >= AdmLogLevel.Warning)
                    {
                        AddLog(MapKind(log.Level), log.Module, "Raw", log.Message);
                    }
                }

                if (outcome.Success)
                {
                    done++;
                    var metric = outcome.LoudnessLufs is { } lu ? $"  ({lu:F1} LUFS)" : "";
                    AddLog(LogKind.Success, "LogDone", "Raw",
                        $"{f.Name} → {Path.GetFileName(outcome.OutputPath ?? outPath)}{metric}");
                    Files.Remove(f); // 成功 → 出队
                }
                else
                {
                    f.Status = FileStatus.Failed;
                    f.Progress = 0;
                    AddLog(LogKind.Error, "LogFailed", "Raw", $"{f.Name}:{outcome.Message}");
                    // 失败 → 沉到队尾(其余文件正常排队)
                    if (Files.Remove(f))
                    {
                        Files.Add(f);
                    }
                }
            }

            if (cancelled)
            {
                SetStatus("StatusCancelled");
                AddLog(LogKind.Warn, "LogCancelled", "LogCancelDetail");
            }
            else
            {
                OverallProgress = 100;
                SetStatus("StatusDone", done, total);
                AddLog(LogKind.Success, "LogAllDone", "LogAllDoneDetail", done, total);
            }
        }
        catch (OperationCanceledException)
        {
            SetStatus("StatusCancelled");
            AddLog(LogKind.Warn, "LogCancelled", "LogCancelDetail");
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

    private RenderSettings BuildSettings()
    {
        var codec = SelectedCodec?.Def;
        uint? opus = null;
        uint? apac = null;
        AdmApacContainer? apacContainer = null;

        if (codec?.Sub == SubOption.BitratePerCh)
        {
            opus = (uint)Math.Clamp(Bitrate, BitrateMin, BitrateMax);
        }
        else if (codec?.Sub == SubOption.BitrateTotal)
        {
            apac = (uint)Math.Clamp(Bitrate, BitrateMin, BitrateMax);
        }

        if (codec?.Id == "apac")
        {
            apacContainer = SelectedContainer?.Id == "apac_caf" ? AdmApacContainer.Caf : AdmApacContainer.Mpeg4;
        }

        return new RenderSettings
        {
            Renderer = SelectedBackend.Renderer,
            Layout = SelectedLayout?.Id ?? "binaural",
            OpusBitratePerChKbps = opus,
            ApacBitrateKbps = apac,
            ApacContainer = apacContainer,
            SofaPath = SofaApplicable ? SofaPath : null, // 仅 SAF 双耳后端传 SOFA
        };
    }

    private string DeriveOutputPath(string input)
    {
        var dir = Path.GetDirectoryName(input) ?? ".";
        var stem = Path.GetFileNameWithoutExtension(input);
        var ext = SelectedContainer?.Ext ?? "wav";
        var path = Path.Combine(dir, $"{stem}.{ext}");
        if (string.Equals(path, input, StringComparison.OrdinalIgnoreCase))
        {
            path = Path.Combine(dir, $"{stem}.rendered.{ext}");
        }

        return path;
    }

    // 结构化进度 → 渲染阶段 i18n key(用 operation 细分);本地化由 RenderFileItem.StageText 完成,
    // 故返回 key 而非翻译串 —— 这样语言切换时正在渲染项也能随 RefreshLanguage 刷新。
    private static string StageWordKey(RenderProgress p) => p.Operation switch
    {
        AdmProgressOperation.ValidateRequest or AdmProgressOperation.ProbeInput => "StageValidate",
        AdmProgressOperation.ImportScene or AdmProgressOperation.ApplySemanticPolicy => "StageImport",
        AdmProgressOperation.PlanRender or AdmProgressOperation.PrepareBackend => "StagePlan",
        AdmProgressOperation.RenderAudio => "StageRender",
        AdmProgressOperation.TrimOutput or AdmProgressOperation.ApplyGain
            or AdmProgressOperation.ConvertBitDepth or AdmProgressOperation.EncodeFlac
            or AdmProgressOperation.EncodeOpus or AdmProgressOperation.EncodeApac
            or AdmProgressOperation.EncodeIamf => "StageEncode",
        AdmProgressOperation.PackageIamfMp4 or AdmProgressOperation.WriteMetadata
            or AdmProgressOperation.Finish => "StagePackage",
        _ => p.Stage switch
        {
            AdmRenderStage.Validating or AdmRenderStage.Probing => "StageValidate",
            AdmRenderStage.ImportingScene => "StageImport",
            AdmRenderStage.Planning => "StagePlan",
            AdmRenderStage.PostProcessing => "StageEncode",
            _ => "StageRender",
        },
    };

    private static LogKind MapKind(AdmLogLevel level) => level switch
    {
        AdmLogLevel.Error => LogKind.Error,
        AdmLogLevel.Warning => LogKind.Warn,
        _ => LogKind.Info,
    };

    // 日志软上限:超过即丢最旧的,防长期多批渲染无限累积(占内存 + 列表变长)。
    private const int MaxLogs = 300;

    // 插到队首:最新日志显示在顶部;title/detail 传 i18n key(detail 空=无详情,或 "Raw"+数据)。
    private void AddLog(LogKind kind, string titleKey, string detailKey = "", params object?[] args)
    {
        Logs.Insert(0, new LogLine(kind, titleKey, detailKey, args, DateTime.Now));
        while (Logs.Count > MaxLogs)
        {
            Logs.RemoveAt(Logs.Count - 1);
        }
    }
}
