using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.ViewModels;

/// <summary>
/// 语义编辑模式 ViewModel(单文档工作台)。载入单个 ADM → adm_inspect_file_json,把对象按 L/R
/// 配对成"行"(一对默认同步编辑,因为 L/R 基本要一起改),每行的听感维度"当前值"为成员区间并集
/// (区间感知)。编辑为相对变换(× scale / dB),行级覆盖按成员展开成多条 id 规则,内存拼
/// mradm.semantic-policy.v1 JSON 供后续试听/渲染消费(#16/#17/#18)。
/// </summary>
public sealed partial class SemanticEditorViewModel : ObservableObject
{
    private static Localizer L => Localizer.Instance;

    public ObservableCollection<SemanticRow> Rows { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasFile))]
    [NotifyPropertyChangedFor(nameof(CanExport))]
    [NotifyPropertyChangedFor(nameof(CanPlay))]
    [NotifyPropertyChangedFor(nameof(LoadedFileName))]
    private string? _loadedPath;

    [ObservableProperty] private SemanticRow? _selectedRow;
    [ObservableProperty] private string _statusText = "";
    [ObservableProperty] private bool _isLoading;

    // 状态文案当前 key + 参数:存 key 而非已求值字符串,切语言时按此重译(与批渲染一致)。
    private string _statusKey = "SemNoFile";
    private object?[] _statusArgs = Array.Empty<object?>();

    private void SetStatus(string key, params object?[] args)
    {
        _statusKey = key;
        _statusArgs = args;
        StatusText = L.Format(key, args);
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasCommonPrefix))]
    [NotifyPropertyChangedFor(nameof(CommonPrefixLabel))]
    private string _commonPrefix = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    private string _policyJson = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    [NotifyPropertyChangedFor(nameof(HasOverrides))]
    private int _overriddenObjectCount;

    public bool HasOverrides => OverriddenObjectCount > 0;

    // 场景对象总数(原始 inspect 计数),显示在「对象」卡片标题旁(从文件状态行挪过来)。
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ObjectCountText))]
    private int _objectCount;

    public string ObjectCountText => ObjectCount > 0 ? L.Format("SemObjectsN", ObjectCount) : "";

    // 空间视图几何模型(载入时构建);独立窗口绑定它 + PlayheadSeconds 做伪 3D 动画。
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanShowSpatial))]
    private SpatialScene? _spatialModel;

    public bool CanShowSpatial => SpatialModel is { IsEmpty: false };

    // 空间视图:是否在对象点旁显示名称(默认开,窗口里可切换)。
    [ObservableProperty]
    private bool _showObjectLabels = true;

    // 空间视图:轨迹模式。false = 拖尾淡出(默认);true = 累积保留(轨迹经过即不消失)。
    [ObservableProperty]
    private bool _persistTrail;

    // 空间视图:角色皮肤(拖入 PNG 设置;null = 内置程序化默认)。记忆上次,启动恢复。
    [ObservableProperty]
    private CharacterSkin? _characterSkin;

    // 拖入皮肤 PNG:加载成功即生效并记忆;失败静默忽略(保持当前皮肤)。
    public void LoadSkin(string path)
    {
        var skin = CharacterSkin.LoadFromPng(path);
        if (skin is null)
        {
            return;
        }

        CharacterSkin = skin;
        SettingsStore.Update(s => s.SkinPath = path);
    }

    public bool HasFile => !string.IsNullOrEmpty(LoadedPath);
    public string LoadedFileName => HasFile ? Path.GetFileName(LoadedPath!) : "";
    public bool HasCommonPrefix => CommonPrefix.Length > 0;
    public string CommonPrefixLabel => HasCommonPrefix ? L.Format("SemPrefix", CommonPrefix) : "";

    public string OverrideSummary =>
        OverriddenObjectCount == 0 ? L["SemNoOverride"] : L.Format("SemOverrideN", OverriddenObjectCount);

    public SemanticEditorViewModel()
    {
        SetStatus("SemNoFile");
        Localizer.Instance.PropertyChanged += (_, _) =>
        {
            SetStatus(_statusKey, _statusArgs); // 按 key 重译当前状态(随语言切换)
            OnPropertyChanged(nameof(OverrideSummary));
            OnPropertyChanged(nameof(CommonPrefixLabel));
            OnPropertyChanged(nameof(ObjectCountText));
            RefreshDevices(); // 「系统默认」项标签随语言切换
        };

        // 监听后端 A/B(默认双耳:拓扑维度需 binaural re-prepare 才听得到)。下拉项为中性专名(后端名在前),
        // 不进 i18n 字典 → 语言无关、不随切换变化(与其它专名下拉一致)。
        MonitorBackends.Add(new MonitorBackendOption("SAF · Binaural", AdmRenderer.SafBinaural, "binaural"));
        MonitorBackends.Add(new MonitorBackendOption("Apple · Binaural", AdmRenderer.Apple, "binaural"));
        _selectedMonitorBackend = MonitorBackends[0];
        var saved = SettingsStore.Load();
        MonitorSofaPath = saved?.MonitorSofaPath;
        MonitorSofa = new SofaSelector(MonitorSofaPath);
        MonitorSofa.SelectionChanged += path => MonitorSofaPath = path;
        // 迁移友好:旧设置仅有 MonitorSofaPath、无 MRU 时,确保它进 MRU 并选中。
        if (!string.IsNullOrEmpty(MonitorSofaPath) && File.Exists(MonitorSofaPath))
        {
            MonitorSofa.Pick(MonitorSofaPath);
        }

        _pendingDeviceId = saved?.MonitorDeviceId ?? "";
        RefreshDevices();

        // 恢复上次拖入的角色皮肤。
        if (saved?.SkinPath is { } skinPath && File.Exists(skinPath))
        {
            CharacterSkin = CharacterSkin.LoadFromPng(skinPath);
        }

        _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) }; // ~30 Hz
        _pollTimer.Tick += (_, _) => PollMonitor();
    }

    public async Task<bool> LoadFileAsync(string path)
    {
        if (string.IsNullOrEmpty(path) || IsLoading)
        {
            return false;
        }

        StopMonitor(); // 换文件先停掉旧监听
        IsLoading = true;
        SetStatus("SemLoading", Path.GetFileName(path));
        try
        {
            var doc = await Task.Run(() =>
            {
                using var ctx = NativeMethods.adm_create_context();
                return ctx.IsInvalid ? null : SemanticInspect.Parse(AdmQueries.FetchInspectJson(ctx, path));
            });

            foreach (var existing in Rows)
            {
                existing.Changed -= OnRowChanged;
            }

            Rows.Clear();
            SelectedRow = null;
            if (doc is null)
            {
                LoadedPath = null;
                CommonPrefix = "";
                ObjectCount = 0;
                SpatialModel = null;
                SetStatus("SemLoadFailed");
                RebuildPolicy();
                return false;
            }

            var names = new List<string>(doc.Objects.Count);
            foreach (var o in doc.Objects)
            {
                if (!string.IsNullOrEmpty(o.Name))
                {
                    names.Add(o.Name);
                }
            }

            CommonPrefix = ObjectNaming.DetectCommonPrefix(names);

            // ADM 里声床(bed)也是 AudioObject,但引用 DirectSpeakers pack(track 里是 ds_blocks 而非
            // object_blocks)。声床照样纳入编辑,但听感维度(diffuse/extent/divergence)是 Objects-only,
            // 对声床无意义 → 只暴露 gain(对象级,实时与导出一致)。无 object_blocks 即判为声床。
            var items = new List<SemanticObjectItem>(doc.Objects.Count);
            foreach (var obj in doc.Objects)
            {
                items.Add(SemanticObjectItem.From(obj, CommonPrefix, isBed: !HasObjectBlocks(obj)));
            }

            foreach (var row in SemanticRow.BuildRows(items))
            {
                row.Changed += OnRowChanged;
                Rows.Add(row);
            }

            LoadedPath = path;
            SelectedRow = Rows.Count > 0 ? Rows[0] : null;
            ObjectCount = doc.Objects.Count;
            SpatialModel = SpatialScene.Build(doc);
            SetStatus("SemLoaded");
            RebuildPolicy();
            return true;
        }
        finally
        {
            IsLoading = false;
        }
    }

    // typeDefinition=Objects 判别:含至少一个带 object_blocks 的 track。声床(DirectSpeakers)
    // 只有 ds_blocks,HOA 走 hoa_tracks 不进 objects → 二者均返回 false,不在语义编辑器列出。
    private static bool HasObjectBlocks(InspectObject obj)
    {
        foreach (var t in obj.Tracks)
        {
            if (t.ObjectBlocks.Count > 0)
            {
                return true;
            }
        }

        return false;
    }

    private void OnRowChanged()
    {
        RebuildPolicy();
        _overridesDirty = true; // 监听中:由轮询定时器去抖后推送(见 PollMonitor)
    }

    // 任一行的覆盖变化 → 重拼整份 policy JSON。行级覆盖按成员展开:一对 L/R = 两条同值 id 规则。
    private void RebuildPolicy()
    {
        var rules = new List<JsonNode?>();
        foreach (var row in Rows)
        {
            foreach (var rule in row.BuildRules())
            {
                rules.Add(rule);
            }
        }

        OverriddenObjectCount = rules.Count;
        if (rules.Count == 0)
        {
            PolicyJson = "";
            return;
        }

        var doc = new JsonObject
        {
            ["schema"] = "mradm.semantic-policy.v1",
            ["objects"] = new JsonArray(rules.ToArray()),
        };
        PolicyJson = doc.ToJsonString();
    }

    /// <summary>清空所有对象的全部覆盖(双击「对象」标题触发)。</summary>
    public void ResetAllOverrides()
    {
        foreach (var row in Rows)
        {
            row.ResetAll();
        }
    }

    // ── 导出生效 ADM(把当前语义覆盖固化写回新 BWF,复用源 PCM;格式选择留给二级窗口的渲染路径) ──

    private readonly AdmRenderService _renderSvc = new();

    [ObservableProperty] private bool _isExporting;

    public bool CanExport => HasFile && !IsExporting;

    /// <summary>把当前 policy 应用到源 ADM,写回到 outputPath。由 View 选好保存路径后调用。返回是否成功。</summary>
    public async Task<bool> ExportToAsync(string outputPath)
    {
        if (!HasFile || IsExporting || string.IsNullOrEmpty(outputPath))
        {
            return false;
        }

        IsExporting = true;
        OnPropertyChanged(nameof(CanExport));
        SetStatus("SemExporting");
        try
        {
            var rc = await _renderSvc.ExportAsync(LoadedPath!, outputPath, string.IsNullOrEmpty(PolicyJson) ? null : PolicyJson);
            bool ok = rc == AdmErrorCode.Ok;
            if (ok)
            {
                SetStatus("SemExported", Path.GetFileName(outputPath));
            }
            else
            {
                SetStatus("SemExportFailed", rc.ToString());
            }

            return ok;
        }
        finally
        {
            IsExporting = false;
            OnPropertyChanged(nameof(CanExport));
        }
    }

    // ── 实时监听(同一份行编辑既驱动 policy,也实时 SetOverrides;契约:monitor 非线程安全 → 全 UI 线程) ──

    private readonly MonitorService _monitor = new();
    private readonly DispatcherTimer _pollTimer;
    private bool _applyingPoll;   // 轮询写 PlayheadSeconds 时置位,避免被当成用户拖动 seek
    private bool _scrubbing;      // 用户正拖动进度条:拖动中只刷新画面、不动引擎,松手才真正 seek 一次
    private bool _overridesDirty; // 行覆盖变更标记,轮询时去抖推送
    private ulong _overrideRevision;
    private uint _sampleRate;

    public ObservableCollection<MonitorBackendOption> MonitorBackends { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MonitorSofaApplicable))]
    [NotifyPropertyChangedFor(nameof(MonitorDiffuseInaudible))]
    private MonitorBackendOption _selectedMonitorBackend;

    // 输出设备选择:列表 = 「系统默认」(Id 空)+ 枚举所得设备。改变时即时切换(监听中)或下次生效。
    public ObservableCollection<MonitorDevice> MonitorDevices { get; } = new();

    [ObservableProperty] private MonitorDevice _selectedMonitorDevice = new("", "", true); // RefreshDevices 立即覆盖

    private string _pendingDeviceId = ""; // 启动时从 settings 恢复的设备 id,RefreshDevices 用它重选

    // 重建设备列表(系统默认 + 枚举),尽量保持当前选择(按 Id)。在构造 / 语言切换 / 开始监听前调用。
    private void RefreshDevices()
    {
        var keepId = SelectedMonitorDevice?.Id ?? _pendingDeviceId;
        MonitorDevices.Clear();
        MonitorDevices.Add(new MonitorDevice("", L["SemDeviceDefault"], true));
        foreach (var d in MonitorService.ListOutputDevices())
        {
            MonitorDevices.Add(d);
        }

        SelectedMonitorDevice = System.Linq.Enumerable.FirstOrDefault(MonitorDevices, d => d.Id == keepId) ??
                                MonitorDevices[0];
    }

    // 选了设备:持久化;监听中即时切换(保留播放头/状态/后端/覆盖)。
    partial void OnSelectedMonitorDeviceChanged(MonitorDevice value)
    {
        if (value is null)
        {
            return;
        }

        SettingsStore.Update(s => s.MonitorDeviceId = value.Id);
        if (IsMonitoring)
        {
            var rc = _monitor.SetOutputDevice(value.Id);
            if (rc != AdmErrorCode.Ok)
            {
                MonitorStatus = L.Format("SemMonSwitchFailed", rc.ToString());
            }
        }
    }

    // 监听用自定义 HRIR(SOFA):只对「双耳·SAF」有效(Apple 双耳用自家 HRTF)。改选择即时重载。
    // MonitorSofa 是 MRU 下拉选择器(默认 + 最近),驱动 MonitorSofaPath(真实路径,下游/持久化消费)。
    [ObservableProperty] private string? _monitorSofaPath;

    public SofaSelector MonitorSofa { get; }

    public bool MonitorSofaApplicable =>
        Models.OutputModel.SofaAvailable && SelectedMonitorBackend?.Renderer == AdmRenderer.SafBinaural;

    // Apple 后端无 ADM 去相关器(supports_diffuse=false):diffuse 改动监听中听不到。
    // 仅作提示——同一份编辑仍写导出 policy,EAR/HOA/SAF 渲染时 diffuse 照常生效,故不禁用控件。
    public bool MonitorDiffuseInaudible => SelectedMonitorBackend?.Renderer == AdmRenderer.Apple;

    // SOFA 路径变了:持久化;监听中且当前是 SAF 双耳 → 用同后端热切换重载新 HRIR。
    partial void OnMonitorSofaPathChanged(string? value)
    {
        SettingsStore.Update(s => s.MonitorSofaPath = value);
        if (IsMonitoring && MonitorSofaApplicable)
        {
            ApplyMonitorBackend(SelectedMonitorBackend);
        }
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanSeek))]
    private bool _isMonitoring;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanSeek))]
    [NotifyPropertyChangedFor(nameof(ShowSpinner))]
    [NotifyPropertyChangedFor(nameof(ShowPlayIcon))]
    [NotifyPropertyChangedFor(nameof(ShowPauseIcon))]
    private bool _isMonitorBusy; // 启动监听中(import + 开设备):播放键显示转圈,点击被忽略

    // 播放器按钮可用态:播放键有文件即可用(启动中点击被 TogglePlayAsync 忽略,但保持启用以显示转圈);
    // 回到开头 / seek 需正在监听。
    public bool CanPlay => HasFile;
    public bool CanSeek => IsMonitoring && !IsMonitorBusy;

    // 播放键三态图标:启动中 = 转圈;否则 播放 / 暂停 二选一。
    public bool ShowSpinner => IsMonitorBusy;
    public bool ShowPlayIcon => !IsMonitorBusy && !IsPlaying;
    public bool ShowPauseIcon => !IsMonitorBusy && IsPlaying;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPlaying))]
    [NotifyPropertyChangedFor(nameof(ShowPlayIcon))]
    [NotifyPropertyChangedFor(nameof(ShowPauseIcon))]
    private AdmMonitorState _monitorState = AdmMonitorState.Stopped;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PlayheadText))]
    private double _playheadSeconds;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DurationText))]
    private double _durationSeconds;

    // 电平表宽度(像素),与 XAML 里的表条宽度一致;填充宽度 = norm*该宽度。
    private const double MeterWidthPx = 150.0;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PeakLeftText))]
    [NotifyPropertyChangedFor(nameof(PeakLeftFillWidth))]
    private float _peakLeft;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PeakRightText))]
    [NotifyPropertyChangedFor(nameof(PeakRightFillWidth))]
    private float _peakRight;

    [ObservableProperty] private string _monitorStatus = "";

    // LUFS(ITU-R BS.1770)三窗读数;UI 一次显示一个,点击在 M / S / I 间循环。
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LufsValueText))]
    private float _momentaryLufs = float.NegativeInfinity;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LufsValueText))]
    private float _shorttermLufs = float.NegativeInfinity;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LufsValueText))]
    private float _integratedLufs = float.NegativeInfinity;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LufsValueText))]
    [NotifyPropertyChangedFor(nameof(LufsModeLabel))]
    private int _lufsMode; // 0 = momentary(M), 1 = short-term(S), 2 = integrated(I)

    public bool IsPlaying => MonitorState == AdmMonitorState.Playing;
    public string PlayheadText => FormatTime(PlayheadSeconds);
    public string DurationText => FormatTime(DurationSeconds);

    // 电平表:线性峰值 → dBFS 文本 + 遮罩宽度。底层是固定的 绿→黄→红 渐变,右侧盖一块遮罩,
    // 宽度 =(1-norm)*表宽——电平越高遮罩越窄、露出越多渐变(越靠近 0 dBFS 越红)。
    public string PeakLeftText => DbfsText(PeakLeft);
    public string PeakRightText => DbfsText(PeakRight);
    public double PeakLeftFillWidth => DbfsNorm(PeakLeft) * MeterWidthPx;
    public double PeakRightFillWidth => DbfsNorm(PeakRight) * MeterWidthPx;

    public string LufsModeLabel => LufsMode switch { 0 => "LUFS-M", 1 => "LUFS-S", _ => "LUFS-I" };

    public string LufsValueText
    {
        get
        {
            var v = LufsMode switch { 0 => MomentaryLufs, 1 => ShorttermLufs, _ => IntegratedLufs };
            return float.IsFinite(v) ? v.ToString("0.0", CultureInfo.InvariantCulture) : "-∞";
        }
    }

    [RelayCommand]
    private void CycleLufsMode() => LufsMode = (LufsMode + 1) % 3;

    private static string FormatTime(double seconds)
    {
        if (double.IsNaN(seconds) || seconds < 0)
        {
            seconds = 0;
        }

        var total = (int)seconds;
        return $"{total / 60}:{total % 60:00}";
    }

    private static string DbfsText(float lin) =>
        lin <= 1.0e-6F ? "-∞" : (20.0 * Math.Log10(lin)).ToString("0.0", CultureInfo.InvariantCulture);

    private static double DbfsNorm(float lin)
    {
        if (lin <= 1.0e-6F)
        {
            return 0.0;
        }

        var db = 20.0 * Math.Log10(lin);
        return Math.Clamp((db + 60.0) / 60.0, 0.0, 1.0);
    }

    // 像常见播放器:主播放键在未监听时自动开监听并播放,之后在播放 / 暂停间切换——永不出现死键。
    [RelayCommand]
    private async Task TogglePlayAsync()
    {
        if (IsMonitorBusy)
        {
            return;
        }

        if (!IsMonitoring)
        {
            await StartMonitorAsync();
            return;
        }

        if (MonitorState == AdmMonitorState.Playing)
        {
            _monitor.Pause();
            MonitorState = AdmMonitorState.Paused;
        }
        else
        {
            _monitor.Play();
            MonitorState = AdmMonitorState.Playing;
        }
    }

    // 回到开头:⏮。
    [RelayCommand]
    private void SeekToStart() => SeekRelative(double.NegativeInfinity);

    // 相对 seek(秒):方向键 ±5s 用。NegativeInfinity = 回到开头。钳到 [0, 时长]。
    public void SeekRelative(double deltaSeconds)
    {
        if (!IsMonitoring)
        {
            return;
        }

        var target = double.IsNegativeInfinity(deltaSeconds)
            ? 0.0
            : Math.Clamp(PlayheadSeconds + deltaSeconds, 0.0, DurationSeconds);
        _monitor.Seek(target);
        PlayheadSeconds = target;
    }

    private async Task StartMonitorAsync()
    {
        if (!HasFile || IsMonitorBusy)
        {
            return;
        }

        var path = LoadedPath!;
        var backend = SelectedMonitorBackend;
        IsMonitorBusy = true;
        MonitorStatus = ""; // 启动状态由播放键转圈体现,不占状态栏文字(避免挤压电平表)

        RefreshDevices(); // 设备可能已插拔 → 开始前刷新列表(保持当前选择)
        var deviceId = SelectedMonitorDevice?.Id ?? "";

        // import + apply policy + 开设备可能略耗时 → 后台启动,await 后回 UI 线程(串行,无并发触 monitor)。
        var settings = MonitorRenderSettings(backend);
        var (rc, sr, dur) = await Task.Run(() =>
        {
            var probe = ProbeDuration(path);
            var start = _monitor.Start(path, settings, deviceId);
            return (start, probe.SampleRate, probe.Duration);
        });

        IsMonitorBusy = false;
        if (rc != AdmErrorCode.Ok)
        {
            MonitorStatus = L.Format("SemMonStartFailed", rc.ToString());
            return;
        }

        _sampleRate = sr;
        DurationSeconds = dur;
        IsMonitoring = true;
        PushOverrides();          // 把当前编辑立即应用到新监听
        _monitor.Play();
        MonitorState = AdmMonitorState.Playing;
        _pollTimer.Start();
    }

    public void StopMonitor()
    {
        _pollTimer.Stop();
        _monitor.Stop();
        _scrubbing = false;
        IsMonitoring = false;
        MonitorState = AdmMonitorState.Stopped;
        PlayheadSeconds = 0;
        PeakLeft = 0;
        PeakRight = 0;
        MomentaryLufs = float.NegativeInfinity;
        ShorttermLufs = float.NegativeInfinity;
        IntegratedLufs = float.NegativeInfinity;
        MonitorStatus = "";
    }

    partial void OnSelectedMonitorBackendChanged(MonitorBackendOption value)
    {
        if (value is not null && IsMonitoring)
        {
            ApplyMonitorBackend(value);
        }
    }

    // 监听用 RenderSettings:SOFA 仅在 SAF 双耳后端带上(Apple 用自家 HRTF)。
    private RenderSettings MonitorRenderSettings(MonitorBackendOption backend) => new()
    {
        Renderer = backend.Renderer,
        Layout = backend.Layout,
        SofaPath = Models.OutputModel.SofaAvailable && backend.Renderer == AdmRenderer.SafBinaural
            ? MonitorSofaPath
            : null,
    };

    // 热切换到指定后端(重载 HRIR / 换后端);失败回显状态。
    private void ApplyMonitorBackend(MonitorBackendOption backend)
    {
        var rc = _monitor.SwitchBackend(MonitorRenderSettings(backend));
        if (rc != AdmErrorCode.Ok)
        {
            MonitorStatus = L.Format("SemMonSwitchFailed", rc.ToString());
        }
    }

    partial void OnPlayheadSecondsChanged(double value)
    {
        if (_applyingPoll || _scrubbing || !IsMonitoring)
        {
            return; // 轮询回写 / 拖动中(只刷新画面,松手才 seek),非即时 seek
        }

        _monitor.Seek(value); // 单击轨道跳转:一次性 ValueChanged → 一次干净 seek
    }

    // 进度条拖动起止(由 View 监听 Thumb 拖动事件驱动):拖动中引擎照旧从原位置播放、不被刷屏式
    // seek 打断(避免碎片拼接的嘈杂);松手时只做一次干净 seek 跳到目标。
    public void BeginScrub() => _scrubbing = true;

    public void EndScrub()
    {
        _scrubbing = false;
        if (IsMonitoring)
        {
            _monitor.Seek(PlayheadSeconds);
        }
    }

    private void PollMonitor()
    {
        if (!IsMonitoring)
        {
            return;
        }

        if (_overridesDirty)
        {
            PushOverrides();
            _overridesDirty = false;
        }

        var st = _monitor.GetStatus();
        if (st is not null)
        {
            // 拖动中不回写进度条:让 thumb 跟手指走,松手 EndScrub 后再恢复跟随引擎播放头。
            if (_sampleRate > 0 && !_scrubbing)
            {
                _applyingPoll = true;
                PlayheadSeconds = st.PlayheadFrames / (double)_sampleRate;
                _applyingPoll = false;
            }

            MonitorState = st.State;                                  // 引擎为准
            MonitorStatus = st.Ended ? L["SemMonEnded"] : st.Failed ? L["SemMonFailed"] : "";
        }

        var lv = _monitor.GetLevels(2);
        if (lv is { Peak.Count: >= 1 })
        {
            // 弹道:瞬时块峰值逐块剧烈起伏,直接画会抽搐 → 快起跳(取新峰)、慢回落(按系数衰减)。
            PeakLeft = SmoothPeak(PeakLeft, lv.Peak[0]);
            PeakRight = SmoothPeak(PeakRight, lv.Peak.Count > 1 ? lv.Peak[1] : lv.Peak[0]);
            MomentaryLufs = lv.MomentaryLufs;
            ShorttermLufs = lv.ShorttermLufs;
            IntegratedLufs = lv.IntegratedLufs;
        }
    }

    // 表头回落系数(每 ~33ms tick):新峰更高则立即跟上,否则向当前值缓慢衰减,避免逐块抖动。
    private const float MeterDecayPerTick = 0.85F;

    private static float SmoothPeak(float prev, float raw) =>
        raw >= prev ? raw : Math.Max(raw, prev * MeterDecayPerTick);

    private void PushOverrides()
    {
        if (!IsMonitoring)
        {
            return;
        }

        var list = new List<MonitorOverride>();
        foreach (var row in Rows)
        {
            list.AddRange(row.BuildLiveOverrides());
        }

        _monitor.SetOverrides(list, ++_overrideRevision);
    }

    private static (uint SampleRate, double Duration) ProbeDuration(string path)
    {
        using var ctx = NativeMethods.adm_create_context();
        if (ctx.IsInvalid)
        {
            return (0, 0);
        }

        var rc = NativeMethods.adm_probe_file(ctx, path, out var info);
        using (info)
        {
            if (rc != AdmErrorCode.Ok || info.IsInvalid)
            {
                return (0, 0);
            }

            return (NativeMethods.adm_scene_info_sample_rate(info), NativeMethods.adm_scene_info_duration_seconds(info));
        }
    }
}

/// <summary>min–max 区间;Max−Min 超阈视为"动态"(逐块变化或成员间差异),否则常量单值。</summary>
public readonly record struct DimRange(double Min, double Max)
{
    public bool IsDynamic => Max - Min > 1e-4;

    public string Format()
    {
        string lo = Min.ToString("0.##", CultureInfo.InvariantCulture);
        return IsDynamic ? $"{lo}–{Max.ToString("0.##", CultureInfo.InvariantCulture)}" : lo;
    }

    public DimRange ClampScaled(double scale, double lo, double hi) =>
        new(Math.Clamp(Min * scale, lo, hi), Math.Clamp(Max * scale, lo, hi));

    public static DimRange Union(DimRange a, DimRange b) => new(Math.Min(a.Min, b.Min), Math.Max(a.Max, b.Max));

    public static DimRange Of(IReadOnlyList<double> values)
    {
        if (values.Count == 0)
        {
            return new DimRange(0, 0);
        }

        double min = values[0];
        double max = values[0];
        foreach (var v in values)
        {
            min = Math.Min(min, v);
            max = Math.Max(max, v);
        }

        return new DimRange(min, max);
    }
}

public interface IResettableOverride
{
    void Reset();
}

/// <summary>
/// 一个标量"覆盖行":[☐ 覆盖] + 值控件。相对变换——ScaleLinear 为逐块 × 系数(钳到 [lo,hi]);
/// GainDb 为 dB 偏移。CurrentText/EffectiveText 即三栏的"当前值"与客户端即时"生效值"。
/// </summary>
public sealed partial class ScalarOverride : ObservableObject, IResettableOverride
{
    public enum Mode
    {
        ScaleLinear,
        GainDb,
    }

    public readonly record struct Axis(string Name, DimRange Range);

    private readonly IReadOnlyList<Axis> _axes;
    private readonly Mode _mode;
    private readonly double _clampLo;
    private readonly double _clampHi;
    private readonly double _defaultValue;

    public string Label { get; }
    public double SliderMin { get; }
    public double SliderMax { get; }
    public event Action? Changed;

    public ScalarOverride(string label, Mode mode, IReadOnlyList<Axis> axes, double defaultValue,
        double sliderMin, double sliderMax, double clampLo = 0.0, double clampHi = double.MaxValue)
    {
        Label = label;
        _mode = mode;
        _axes = axes;
        _value = defaultValue;
        _defaultValue = defaultValue;
        SliderMin = sliderMin;
        SliderMax = sliderMax;
        _clampLo = clampLo;
        _clampHi = clampHi;
    }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(EffectiveText))]
    private bool _enabled;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(EffectiveText))]
    [NotifyPropertyChangedFor(nameof(ValueText))]
    [NotifyPropertyChangedFor(nameof(ValueEntry))]
    private double _value;

    partial void OnEnabledChanged(bool value) => Changed?.Invoke();
    partial void OnValueChanged(double value) => Changed?.Invoke();

    /// <summary>清掉本维度覆盖:取消勾选 + 值回到中性默认(gain 0 dB / scale ×1)。</summary>
    public void Reset()
    {
        Enabled = false;
        Value = _defaultValue;
    }

    // 单位装饰(贴在可输入框两侧):scale 用前缀 ×,gain 用后缀 dB。
    public string UnitPrefix => _mode == Mode.ScaleLinear ? "×" : "";
    public string UnitSuffix => _mode == Mode.GainDb ? "dB" : "";

    /// <summary>覆盖值的可编辑文本(只含数字,不含单位)。提交时解析 + 钳到滑块范围,并自动勾选
    /// 启用——直接键入数值即视为要应用该覆盖(与拖滑块同效,但免去先勾选)。</summary>
    public string ValueEntry
    {
        get => _mode == Mode.GainDb
            ? Value.ToString("0.0", CultureInfo.InvariantCulture)
            : Value.ToString("0.00", CultureInfo.InvariantCulture);
        set
        {
            if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
            {
                Enabled = true;
                Value = Math.Clamp(v, SliderMin, SliderMax);
            }

            OnPropertyChanged(); // 回写规范化后的文本(拒绝非法输入 / 反映钳制结果)
        }
    }

    public string ValueText => _mode == Mode.GainDb
        ? Value.ToString("+0.0;-0.0;0", CultureInfo.InvariantCulture) + " dB"
        : "×" + Value.ToString("0.00", CultureInfo.InvariantCulture);

    public string CurrentText => Render(scaled: false);
    public string EffectiveText => Enabled ? Render(scaled: true) : CurrentText;

    private string Render(bool scaled)
    {
        if (_mode == Mode.GainDb)
        {
            double db = LinToDb(_axes[0].Range.Min) + (scaled ? Value : 0.0);
            return db.ToString("+0.0;-0.0;0", CultureInfo.InvariantCulture) + " dB";
        }

        if (_axes.Count == 1)
        {
            var r = scaled ? _axes[0].Range.ClampScaled(Value, _clampLo, _clampHi) : _axes[0].Range;
            return r.Format();
        }

        var parts = new List<string>(_axes.Count);
        foreach (var ax in _axes)
        {
            var r = scaled ? ax.Range.ClampScaled(Value, _clampLo, _clampHi) : ax.Range;
            parts.Add($"{ax.Name} {r.Format()}");
        }

        return string.Join("  ", parts);
    }

    /// <summary>启用时写出该维度的 policy 片段(scale 或 gain_db);未启用返回 null。每次新建实例。</summary>
    public JsonObject? ToPolicyFragment()
    {
        if (!Enabled)
        {
            return null;
        }

        return _mode == Mode.GainDb
            ? new JsonObject { ["gain_db"] = Value }
            : new JsonObject { ["scale"] = Value };
    }

    private static double LinToDb(double linear) => linear <= 1.0e-6 ? -120.0 : 20.0 * Math.Log10(linear);
}

/// <summary>extent 的三个常驻独立轴(Width/Height/Depth,各为一条标准覆盖行)+ 两个相邻链(W↔H、H↔D)。
/// 链开启时,编辑链所连通的任一轴会把值 / 启用同步到同组各轴(两链皆开 = 三轴齐动)。</summary>
public sealed partial class ExtentOverride : ObservableObject, IResettableOverride
{
    public ScalarOverride Width { get; }
    public ScalarOverride Height { get; }
    public ScalarOverride Depth { get; }

    private readonly ScalarOverride[] _axes;
    private bool _syncing;

    public event Action? Changed;

    public ExtentOverride(DimRange width, DimRange height, DimRange depth)
    {
        Width = MakeAxis("Width", width);
        Height = MakeAxis("Height", height);
        Depth = MakeAxis("Depth", depth);
        _axes = new[] { Width, Height, Depth };
        for (int i = 0; i < _axes.Length; i++)
        {
            int idx = i;
            _axes[i].Changed += () => OnAxisChanged(idx);
        }
    }

    private static ScalarOverride MakeAxis(string label, DimRange range) => new(label, ScalarOverride.Mode.ScaleLinear,
        new[] { new ScalarOverride.Axis("", range) }, 1.0, 0.0, 4.0, 0.0, 1.0);

    // 每轴一个"联动"标志(单击轴名切换)。所有联动轴共享同一值 / 启用;未联动轴独立。
    [ObservableProperty] private bool _widthLinked = true;
    [ObservableProperty] private bool _heightLinked = true;
    [ObservableProperty] private bool _depthLinked = true;

    public bool AnyEnabled => Width.Enabled || Height.Enabled || Depth.Enabled;

    partial void OnWidthLinkedChanged(bool value) => OnLinkToggled(0, value);
    partial void OnHeightLinkedChanged(bool value) => OnLinkToggled(1, value);
    partial void OnDepthLinkedChanged(bool value) => OnLinkToggled(2, value);

    private bool[] LinkedFlags => new[] { WidthLinked, HeightLinked, DepthLinked };

    // 接入联动组时,该轴吸附到组内已有联动轴的当前值(以第一个其它联动轴为锚)。
    private void OnLinkToggled(int idx, bool linked)
    {
        if (linked)
        {
            var flags = LinkedFlags;
            for (int k = 0; k < _axes.Length; k++)
            {
                if (k != idx && flags[k])
                {
                    _syncing = true;
                    _axes[idx].Value = _axes[k].Value;
                    _axes[idx].Enabled = _axes[k].Enabled;
                    _syncing = false;
                    break;
                }
            }
        }

        Changed?.Invoke();
    }

    public void Reset()
    {
        _syncing = true;
        Width.Reset();
        Height.Reset();
        Depth.Reset();
        _syncing = false;
        WidthLinked = true;
        HeightLinked = true;
        DepthLinked = true;
        Changed?.Invoke();
    }

    /// <summary>启用的轴各产一个 *_scale;三轴全启用且相等时折叠成单个 scale。未启用的轴不写(保持不变)。</summary>
    public JsonObject? ToPolicyFragment()
    {
        if (!AnyEnabled)
        {
            return null;
        }

        if (Width.Enabled && Height.Enabled && Depth.Enabled && NearlyEqual(Width.Value, Height.Value) &&
            NearlyEqual(Width.Value, Depth.Value))
        {
            return new JsonObject { ["scale"] = Width.Value };
        }

        var o = new JsonObject();
        if (Width.Enabled)
        {
            o["width_scale"] = Width.Value;
        }

        if (Height.Enabled)
        {
            o["height_scale"] = Height.Value;
        }

        if (Depth.Enabled)
        {
            o["depth_scale"] = Depth.Value;
        }

        return o;
    }

    // 编辑某联动轴 → 把它的 value + enabled 同步到其它所有联动轴(未联动轴不受影响)。
    private void OnAxisChanged(int idx)
    {
        var flags = LinkedFlags;
        if (!_syncing && flags[idx])
        {
            _syncing = true;
            var src = _axes[idx];
            for (int j = 0; j < _axes.Length; j++)
            {
                if (j != idx && flags[j])
                {
                    _axes[j].Value = src.Value;
                    _axes[j].Enabled = src.Enabled;
                }
            }

            _syncing = false;
        }

        Changed?.Invoke();
    }

    private static bool NearlyEqual(double a, double b) => Math.Abs(a - b) <= 1.0e-6;
}

/// <summary>声床一个声道的纯数据:speaker label(命中键)+ 当前 gain(线性,= ds.gain × object.gain)
/// + 位置(用于按相反方位角做稳健的 L/R 配对,而非脆弱的标签字符串匹配)。</summary>
public readonly record struct BedChannelData(
    string SpeakerLabel, double GainLinear, bool HasPosition, double Azimuth, double Elevation);

/// <summary>声床的一个编辑单元:单声道或一对 L/R(同步编辑,共享一个 gain)。SpeakerLabels 含 1~2 个
/// 标签,导出 / 实时各自按标签展开成同值规则。DisplayLabel 为剥掉公共前缀后的显示名。</summary>
public sealed class BedChannelGroup
{
    public IReadOnlyList<string> SpeakerLabels { get; }
    public string DisplayLabel { get; }
    public ScalarOverride Gain { get; }

    public BedChannelGroup(IReadOnlyList<string> speakerLabels, string displayLabel, double currentGainLinear)
    {
        SpeakerLabels = speakerLabels;
        DisplayLabel = displayLabel;
        Gain = new ScalarOverride(displayLabel, ScalarOverride.Mode.GainDb,
            new[] { new ScalarOverride.Axis("", new DimRange(currentGainLinear, currentGainLinear)) }, 0.0, -24.0, 12.0);
    }
}

/// <summary>
/// 单个对象的纯数据:身份 + 各听感维度的"当前值"区间(扫全块求 min/max,权威源 inspect)。
/// 不持有覆盖——覆盖在 SemanticRow 层(行=1~2 个对象,L/R 对共享一份覆盖以同步编辑)。
/// </summary>
public sealed class SemanticObjectItem
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public required string DisplayName { get; init; }
    public int? Importance { get; init; }
    public int? DialogueId { get; init; }
    public bool Mute { get; init; }

    // typeDefinition=DirectSpeakers(声床/bed):只 gain 可编辑,听感维度不适用。
    public bool IsBed { get; init; }

    // 声床各声道(每 track 一个:speaker label + 当前 gain 线性)。Objects 为空。按声道独立 gain 用。
    public IReadOnlyList<BedChannelData> BedChannels { get; init; } = System.Array.Empty<BedChannelData>();

    public DimRange GainRange { get; init; }
    public DimRange DiffuseRange { get; init; }
    public DimRange WidthRange { get; init; }
    public DimRange HeightRange { get; init; }
    public DimRange DepthRange { get; init; }
    public DimRange DivergenceRange { get; init; }

    public string Tooltip => string.IsNullOrEmpty(Name) ? Id : $"{Name}  ({Id})";

    public string Summary =>
        $"gain {GainRange.Format()} · diffuse {DiffuseRange.Format()} · " +
        $"extent {WidthRange.Format()}/{HeightRange.Format()}/{DepthRange.Format()} · " +
        $"divergence {DivergenceRange.Format()}";

    public string Meta
    {
        get
        {
            var parts = new List<string>();
            if (IsBed)
            {
                parts.Add("bed");
            }

            if (Importance is { } imp)
            {
                parts.Add($"importance {imp}");
            }

            if (DialogueId is { } dlg)
            {
                parts.Add($"dialogue {dlg}");
            }

            if (Mute)
            {
                parts.Add("mute");
            }

            return string.Join(" · ", parts);
        }
    }

    internal static SemanticObjectItem From(InspectObject obj, string commonPrefix = "", bool isBed = false)
    {
        var diffuse = new List<double>();
        var width = new List<double>();
        var height = new List<double>();
        var depth = new List<double>();
        var divergence = new List<double>();
        var bedChannels = new List<BedChannelData>();
        foreach (var track in obj.Tracks)
        {
            foreach (var b in track.ObjectBlocks)
            {
                diffuse.Add(b.Diffuse);
                width.Add(b.Width);
                height.Add(b.Height);
                depth.Add(b.Depth);
                divergence.Add(b.Divergence);
            }

            // Bed channels: each DirectSpeakers track is one speaker channel. Take the first ds_block's
            // first label as the channel identity (matches the renderer's per-channel live-gain key).
            if (isBed && track.DsBlocks.Count > 0 && track.DsBlocks[0].SpeakerLabels.Count > 0)
            {
                var ds = track.DsBlocks[0];
                bedChannels.Add(new BedChannelData(
                    ds.SpeakerLabels[0], ds.Gain * obj.Gain, ds.HasPosition, ds.Azimuth, ds.Elevation));
            }
        }

        return new SemanticObjectItem
        {
            Id = obj.Id,
            Name = obj.Name,
            DisplayName = ObjectNaming.Strip(string.IsNullOrEmpty(obj.Name) ? obj.Id : obj.Name, commonPrefix),
            IsBed = isBed,
            BedChannels = bedChannels,
            Importance = obj.Importance,
            DialogueId = obj.DialogueId,
            Mute = obj.Mute,
            GainRange = new DimRange(obj.Gain, obj.Gain),
            DiffuseRange = DimRange.Of(diffuse),
            WidthRange = DimRange.Of(width),
            HeightRange = DimRange.Of(height),
            DepthRange = DimRange.Of(depth),
            DivergenceRange = DimRange.Of(divergence),
        };
    }
}

/// <summary>
/// 编辑/展示单元:1 个对象,或一个 L/R 对(同步编辑)。听感维度区间为成员并集;一份覆盖,
/// 序列化时按成员展开成多条同值 id 规则。
/// </summary>
public sealed class SemanticRow : ObservableObject
{
    public IReadOnlyList<SemanticObjectItem> Members { get; }
    public string DisplayName { get; }
    public string ChannelTag { get; }

    // 本行是否已设任意覆盖(供左侧列表把已改对象变色)。随覆盖变化实时通知。
    public bool HasOverride
    {
        get
        {
            if (GainDb.Enabled || DiffuseScale.Enabled || DivergenceScale.Enabled || Extent.AnyEnabled)
            {
                return true;
            }

            foreach (var bc in BedChannels)
            {
                if (bc.Gain.Enabled)
                {
                    return true;
                }
            }

            return false;
        }
    }

    // 声床行:diffuse/extent/divergence(Objects-only)隐藏;gain 按声道独立(BedChannels),
    // 而非整对象一个 GainDb。Objects 行反之:用 GainDb + 听感维度,BedChannels 为空。
    public bool IsBed { get; }
    public bool ShowPerceptual => !IsBed;
    public bool ShowObjectGain => !IsBed; // 声床用按声道 gain 取代整对象 gain 行
    public IReadOnlyList<BedChannelGroup> BedChannels { get; }

    public ScalarOverride GainDb { get; }
    public ScalarOverride DiffuseScale { get; }
    public ExtentOverride Extent { get; }
    public ScalarOverride DivergenceScale { get; }

    public event Action? Changed;

    private SemanticRow(IReadOnlyList<SemanticObjectItem> members, string displayName, string channelTag)
    {
        Members = members;
        DisplayName = displayName;
        ChannelTag = channelTag;
        IsBed = members.Count > 0 && members[0].IsBed;

        var bedChannels = IsBed ? BuildBedGroups(members[0].BedChannels) : new List<BedChannelGroup>();
        foreach (var bc in bedChannels)
        {
            bc.Gain.Changed += OnChildChanged;
        }

        BedChannels = bedChannels;

        DimRange Union(Func<SemanticObjectItem, DimRange> sel)
        {
            var r = sel(members[0]);
            for (int k = 1; k < members.Count; k++)
            {
                r = DimRange.Union(r, sel(members[k]));
            }

            return r;
        }

        GainDb = new ScalarOverride("gain", ScalarOverride.Mode.GainDb,
            new[] { new ScalarOverride.Axis("", Union(m => m.GainRange)) }, 0.0, -24.0, 12.0);
        DiffuseScale = new ScalarOverride("diffuse", ScalarOverride.Mode.ScaleLinear,
            new[] { new ScalarOverride.Axis("", Union(m => m.DiffuseRange)) }, 1.0, 0.0, 4.0, 0.0, 1.0);
        Extent = new ExtentOverride(Union(m => m.WidthRange), Union(m => m.HeightRange), Union(m => m.DepthRange));
        DivergenceScale = new ScalarOverride("divergence", ScalarOverride.Mode.ScaleLinear,
            new[] { new ScalarOverride.Axis("", Union(m => m.DivergenceRange)) }, 1.0, 0.0, 4.0, 0.0, 1.0);

        GainDb.Changed += OnChildChanged;
        DiffuseScale.Changed += OnChildChanged;
        Extent.Changed += OnChildChanged;
        DivergenceScale.Changed += OnChildChanged;
    }

    // 任一子覆盖变化:对外抛 Changed(驱动 policy / 监听),并刷新 HasOverride(列表变色)。
    private void OnChildChanged()
    {
        Changed?.Invoke();
        OnPropertyChanged(nameof(HasOverride));
    }

    /// <summary>清空本行(对象 / L·R 对 / 声床)全部覆盖。</summary>
    public void ResetAll()
    {
        GainDb.Reset();
        DiffuseScale.Reset();
        Extent.Reset();
        DivergenceScale.Reset();
        foreach (var bc in BedChannels)
        {
            bc.Gain.Reset();
        }
    }

    public string Tooltip
    {
        get
        {
            if (Members.Count == 1)
            {
                return Members[0].Tooltip;
            }

            var lines = new List<string>(Members.Count);
            foreach (var m in Members)
            {
                lines.Add(m.Tooltip);
            }

            return string.Join("\n", lines);
        }
    }

    public string Meta => Members[0].Meta;

    /// <summary>每个成员一条 id 规则(行覆盖按成员展开;一对 L/R = 两条同值规则)。无覆盖则不产出。</summary>
    public IEnumerable<JsonObject> BuildRules()
    {
        // 声床:按声道产 direct_speakers 规则({id, direct_speakers:{speaker_label, gain:{gain_db}}})。
        if (IsBed)
        {
            foreach (var bc in BedChannels)
            {
                // 配对组按各成员声道展开:每个 speaker_label 一条同值 direct_speakers 规则。
                foreach (var label in bc.SpeakerLabels)
                {
                    var gain = bc.Gain.ToPolicyFragment(); // 每次新建实例,可安全挂到不同规则
                    if (gain is null)
                    {
                        continue;
                    }

                    yield return new JsonObject
                    {
                        ["id"] = Members[0].Id,
                        ["direct_speakers"] = new JsonObject
                        {
                            ["speaker_label"] = label,
                            ["gain"] = gain,
                        },
                    };
                }
            }

            yield break;
        }

        foreach (var m in Members)
        {
            var rule = new JsonObject();
            AddIf(rule, "gain", GainDb.ToPolicyFragment());
            AddIf(rule, "diffuse", DiffuseScale.ToPolicyFragment());
            AddIf(rule, "extent", Extent.ToPolicyFragment());
            AddIf(rule, "divergence", DivergenceScale.ToPolicyFragment());
            if (rule.Count == 0)
            {
                continue;
            }

            rule["id"] = m.Id;
            yield return rule;
        }
    }

    private static void AddIf(JsonObject rule, string key, JsonObject? fragment)
    {
        if (fragment is not null)
        {
            rule[key] = fragment;
        }
    }

    /// <summary>把本行覆盖投影成实时监听覆盖,按成员展开(每对象一条)。无启用维度则不产出。
    /// 与 BuildRules 同源:gain=dB 偏移(未启用 0),其余=× 倍(未启用 1.0)。</summary>
    public IEnumerable<MonitorOverride> BuildLiveOverrides()
    {
        // 声床:每个启用的声道一条 override,带 speaker_label 命中该声道(实时按声道生效)。
        if (IsBed)
        {
            foreach (var bc in BedChannels)
            {
                if (!bc.Gain.Enabled)
                {
                    continue;
                }

                foreach (var label in bc.SpeakerLabels) // 配对组:每声道一条带 speaker_label 的覆盖
                {
                    yield return new MonitorOverride(Members[0].Id, (float)bc.Gain.Value, SpeakerLabel: label);
                }
            }

            yield break;
        }

        if (!GainDb.Enabled && !DiffuseScale.Enabled && !Extent.AnyEnabled && !DivergenceScale.Enabled)
        {
            yield break;
        }

        var gainDb = (float)(GainDb.Enabled ? GainDb.Value : 0.0);
        var diffuse = (float)(DiffuseScale.Enabled ? DiffuseScale.Value : 1.0);
        var extentWidth = (float)(Extent.Width.Enabled ? Extent.Width.Value : 1.0);
        var extentHeight = (float)(Extent.Height.Enabled ? Extent.Height.Value : 1.0);
        var extentDepth = (float)(Extent.Depth.Enabled ? Extent.Depth.Value : 1.0);
        var divergence = (float)(DivergenceScale.Enabled ? DivergenceScale.Value : 1.0);
        foreach (var m in Members)
        {
            yield return new MonitorOverride(m.Id, gainDb, diffuse, 1.0f, divergence, extentWidth, extentHeight, extentDepth);
        }
    }

    // 把声床声道按"方位角相反、仰角相同"两两配成 L/R(共享一个 gain);中置 / LFE / 无对侧者单列。
    // 用位置而非标签字符串,避开 RC_LFE 之类标签含 "L" 的误判。显示名剥掉公共前缀(如有)。
    private static List<BedChannelGroup> BuildBedGroups(IReadOnlyList<BedChannelData> channels)
    {
        var labels = new List<string>(channels.Count);
        foreach (var c in channels)
        {
            labels.Add(c.SpeakerLabel);
        }

        var prefix = CommonLabelPrefix(labels); // 剥掉如 "RC_" 的公共前缀,避免 80px 列截断
        string Display(BedChannelData c) => ObjectNaming.Strip(c.SpeakerLabel, prefix);

        const double azTol = 1.0;
        const double elTol = 1.0;
        var groups = new List<BedChannelGroup>();
        var used = new bool[channels.Count];
        for (int i = 0; i < channels.Count; i++)
        {
            if (used[i])
            {
                continue;
            }

            var ci = channels[i];
            int partner = -1;
            if (ci.HasPosition && Math.Abs(ci.Azimuth) > azTol) // 中置(az≈0)不配对
            {
                for (int j = i + 1; j < channels.Count; j++)
                {
                    if (used[j])
                    {
                        continue;
                    }

                    var cj = channels[j];
                    if (cj.HasPosition && Math.Abs(ci.Azimuth + cj.Azimuth) < azTol &&
                        Math.Abs(ci.Elevation - cj.Elevation) < elTol)
                    {
                        partner = j;
                        break;
                    }
                }
            }

            if (partner >= 0)
            {
                used[i] = true;
                used[partner] = true;
                // 左 = 方位角为正(BS.2051:+az 在左)。
                var left = ci.Azimuth >= 0 ? ci : channels[partner];
                var right = ci.Azimuth >= 0 ? channels[partner] : ci;
                groups.Add(new BedChannelGroup(new[] { left.SpeakerLabel, right.SpeakerLabel },
                    $"{Display(left)} · {Display(right)}", left.GainLinear));
            }
            else
            {
                used[i] = true;
                groups.Add(new BedChannelGroup(new[] { ci.SpeakerLabel }, Display(ci), ci.GainLinear));
            }
        }

        return groups;
    }

    // 声床标签的公共前缀(切在分隔符 _ - 空格 / 边界,无最小长度限制 → 能剥掉短前缀如 "RC_";
    // 无分隔符边界则返回空,如 "M+030"/"M-030" 公共串 "M" 不剥)。仅用于显示。
    private static string CommonLabelPrefix(IReadOnlyList<string> labels)
    {
        if (labels.Count < 2)
        {
            return "";
        }

        var common = labels[0];
        foreach (var l in labels)
        {
            int n = Math.Min(common.Length, l.Length);
            int k = 0;
            while (k < n && common[k] == l[k])
            {
                k++;
            }

            common = common[..k];
        }

        int cut = common.LastIndexOfAny(new[] { '_', '-', ' ', '/' });
        return cut >= 0 ? common[..(cut + 1)] : "";
    }

    /// <summary>把对象按 L/R 配对成行(同 stem、相反声道、各一个);其余为单成员行。保留文档顺序。</summary>
    public static List<SemanticRow> BuildRows(IReadOnlyList<SemanticObjectItem> items)
    {
        var rows = new List<SemanticRow>();
        var used = new bool[items.Count];
        for (int i = 0; i < items.Count; i++)
        {
            if (used[i])
            {
                continue;
            }

            var (stem, channel) = ObjectNaming.SplitChannel(items[i].DisplayName);
            if (channel != '\0')
            {
                int partner = -1;
                for (int j = i + 1; j < items.Count; j++)
                {
                    if (used[j])
                    {
                        continue;
                    }

                    var (stemJ, channelJ) = ObjectNaming.SplitChannel(items[j].DisplayName);
                    if (channelJ != '\0' && channelJ != channel &&
                        string.Equals(stem, stemJ, StringComparison.OrdinalIgnoreCase))
                    {
                        partner = j;
                        break;
                    }
                }

                if (partner >= 0)
                {
                    used[i] = true;
                    used[partner] = true;
                    var left = channel == 'L' ? items[i] : items[partner];
                    var right = channel == 'L' ? items[partner] : items[i];
                    rows.Add(new SemanticRow(new[] { left, right }, stem, "L · R"));
                    continue;
                }
            }

            used[i] = true;
            rows.Add(new SemanticRow(new[] { items[i] }, items[i].DisplayName, ""));
        }

        return rows;
    }
}
