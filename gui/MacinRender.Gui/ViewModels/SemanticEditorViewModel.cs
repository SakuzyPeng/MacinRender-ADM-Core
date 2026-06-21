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

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasCommonPrefix))]
    [NotifyPropertyChangedFor(nameof(CommonPrefixLabel))]
    private string _commonPrefix = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    private string _policyJson = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    private int _overriddenObjectCount;

    public bool HasFile => !string.IsNullOrEmpty(LoadedPath);
    public string LoadedFileName => HasFile ? Path.GetFileName(LoadedPath!) : "";
    public bool HasCommonPrefix => CommonPrefix.Length > 0;
    public string CommonPrefixLabel => HasCommonPrefix ? L.Format("SemPrefix", CommonPrefix) : "";

    public string OverrideSummary =>
        OverriddenObjectCount == 0 ? L["SemNoOverride"] : L.Format("SemOverrideN", OverriddenObjectCount);

    public SemanticEditorViewModel()
    {
        StatusText = L["SemNoFile"];
        Localizer.Instance.PropertyChanged += (_, _) =>
        {
            if (!HasFile)
            {
                StatusText = L["SemNoFile"];
            }

            OnPropertyChanged(nameof(OverrideSummary));
            OnPropertyChanged(nameof(CommonPrefixLabel));
        };

        // 监听后端 A/B(默认双耳:拓扑维度需 binaural re-prepare 才听得到)。下拉项为中性专名,免翻。
        MonitorBackends.Add(new MonitorBackendOption("双耳 · SAF", AdmRenderer.SafBinaural, "binaural"));
        MonitorBackends.Add(new MonitorBackendOption("双耳 · Apple", AdmRenderer.Apple, "binaural"));
        _selectedMonitorBackend = MonitorBackends[0];
        MonitorSofaPath = SettingsStore.Load()?.MonitorSofaPath;

        _pollTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) }; // ~30 Hz
        _pollTimer.Tick += (_, _) => PollMonitor();
    }

    public async Task LoadFileAsync(string path)
    {
        if (string.IsNullOrEmpty(path) || IsLoading)
        {
            return;
        }

        StopMonitor(); // 换文件先停掉旧监听
        IsLoading = true;
        StatusText = L.Format("SemLoading", Path.GetFileName(path));
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
                StatusText = L["SemLoadFailed"];
                RebuildPolicy();
                return;
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

            var items = new List<SemanticObjectItem>(doc.Objects.Count);
            foreach (var obj in doc.Objects)
            {
                items.Add(SemanticObjectItem.From(obj, CommonPrefix));
            }

            foreach (var row in SemanticRow.BuildRows(items))
            {
                row.Changed += OnRowChanged;
                Rows.Add(row);
            }

            LoadedPath = path;
            SelectedRow = Rows.Count > 0 ? Rows[0] : null;
            StatusText = L.Format("SemLoaded", doc.Objects.Count);
            RebuildPolicy();
        }
        finally
        {
            IsLoading = false;
        }
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

    /// <summary>把当前 policy 应用到源 ADM,写回到 outputPath。由 View 选好保存路径后调用。</summary>
    public async Task ExportToAsync(string outputPath)
    {
        if (!HasFile || IsExporting || string.IsNullOrEmpty(outputPath))
        {
            return;
        }

        IsExporting = true;
        OnPropertyChanged(nameof(CanExport));
        StatusText = L["SemExporting"];
        try
        {
            var rc = await _renderSvc.ExportAsync(LoadedPath!, outputPath, string.IsNullOrEmpty(PolicyJson) ? null : PolicyJson);
            StatusText = rc == AdmErrorCode.Ok
                ? L.Format("SemExported", Path.GetFileName(outputPath))
                : L.Format("SemExportFailed", rc.ToString());
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
    private bool _overridesDirty; // 行覆盖变更标记,轮询时去抖推送
    private ulong _overrideRevision;
    private uint _sampleRate;

    public ObservableCollection<MonitorBackendOption> MonitorBackends { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(MonitorSofaApplicable))]
    private MonitorBackendOption _selectedMonitorBackend;

    // 监听用自定义 HRIR(SOFA):只对「双耳·SAF」有效(Apple 双耳用自家 HRTF)。改路径即时重载。
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasMonitorSofa))]
    [NotifyPropertyChangedFor(nameof(MonitorSofaFileName))]
    [NotifyPropertyChangedFor(nameof(MonitorSofaSummary))]
    private string? _monitorSofaPath;

    public bool MonitorSofaApplicable =>
        Models.OutputModel.SofaAvailable && SelectedMonitorBackend?.Renderer == AdmRenderer.SafBinaural;
    public bool HasMonitorSofa => !string.IsNullOrEmpty(MonitorSofaPath);
    public string MonitorSofaFileName => HasMonitorSofa ? Path.GetFileNameWithoutExtension(MonitorSofaPath!) : "";
    public string MonitorSofaSummary => HasMonitorSofa ? $"HRIR: {MonitorSofaFileName}" : L["SemSofaNone"];

    public void SetMonitorSofa(string path) => MonitorSofaPath = path;

    [RelayCommand]
    private void ClearMonitorSofa() => MonitorSofaPath = null;

    // SOFA 路径变了:监听中且当前是 SAF 双耳 → 用同后端热切换重载新 HRIR。
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
    [NotifyPropertyChangedFor(nameof(CanPlay))]
    private bool _isMonitorBusy; // 启动监听中(import + 开设备),按钮禁用

    // 播放器按钮可用态:播放键需有文件且不在启动中;回到开头 / seek 需正在监听。
    public bool CanPlay => HasFile && !IsMonitorBusy;
    public bool CanSeek => IsMonitoring && !IsMonitorBusy;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPlaying))]
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
        MonitorStatus = L["SemMonStarting"];

        // import + apply policy + 开设备可能略耗时 → 后台启动,await 后回 UI 线程(串行,无并发触 monitor)。
        var settings = MonitorRenderSettings(backend);
        var (rc, sr, dur) = await Task.Run(() =>
        {
            var probe = ProbeDuration(path);
            var start = _monitor.Start(path, settings);
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
        if (_applyingPoll || !IsMonitoring)
        {
            return; // 轮询回写,非用户拖动
        }

        _monitor.Seek(value);
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
            if (_sampleRate > 0)
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

/// <summary>
/// 一个标量"覆盖行":[☐ 覆盖] + 值控件。相对变换——ScaleLinear 为逐块 × 系数(钳到 [lo,hi]);
/// GainDb 为 dB 偏移。CurrentText/EffectiveText 即三栏的"当前值"与客户端即时"生效值"。
/// </summary>
public sealed partial class ScalarOverride : ObservableObject
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

    internal static SemanticObjectItem From(InspectObject obj, string commonPrefix = "")
    {
        var diffuse = new List<double>();
        var width = new List<double>();
        var height = new List<double>();
        var depth = new List<double>();
        var divergence = new List<double>();
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
        }

        return new SemanticObjectItem
        {
            Id = obj.Id,
            Name = obj.Name,
            DisplayName = ObjectNaming.Strip(string.IsNullOrEmpty(obj.Name) ? obj.Id : obj.Name, commonPrefix),
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
public sealed class SemanticRow
{
    public IReadOnlyList<SemanticObjectItem> Members { get; }
    public string DisplayName { get; }
    public string ChannelTag { get; }

    public ScalarOverride GainDb { get; }
    public ScalarOverride DiffuseScale { get; }
    public ScalarOverride ExtentScale { get; }
    public ScalarOverride DivergenceScale { get; }

    public event Action? Changed;

    private SemanticRow(IReadOnlyList<SemanticObjectItem> members, string displayName, string channelTag)
    {
        Members = members;
        DisplayName = displayName;
        ChannelTag = channelTag;

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
        ExtentScale = new ScalarOverride("extent", ScalarOverride.Mode.ScaleLinear,
            new[]
            {
                new ScalarOverride.Axis("W", Union(m => m.WidthRange)),
                new ScalarOverride.Axis("H", Union(m => m.HeightRange)),
                new ScalarOverride.Axis("D", Union(m => m.DepthRange)),
            }, 1.0, 0.0, 4.0);
        DivergenceScale = new ScalarOverride("divergence", ScalarOverride.Mode.ScaleLinear,
            new[] { new ScalarOverride.Axis("", Union(m => m.DivergenceRange)) }, 1.0, 0.0, 4.0, 0.0, 1.0);

        foreach (var ov in new[] { GainDb, DiffuseScale, ExtentScale, DivergenceScale })
        {
            ov.Changed += () => Changed?.Invoke();
        }
    }

    /// <summary>清空本行(对象 / L·R 对)全部维度覆盖。</summary>
    public void ResetAll()
    {
        GainDb.Reset();
        DiffuseScale.Reset();
        ExtentScale.Reset();
        DivergenceScale.Reset();
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
        foreach (var m in Members)
        {
            var rule = new JsonObject();
            AddIf(rule, "gain", GainDb.ToPolicyFragment());
            AddIf(rule, "diffuse", DiffuseScale.ToPolicyFragment());
            AddIf(rule, "extent", ExtentScale.ToPolicyFragment());
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
        if (!GainDb.Enabled && !DiffuseScale.Enabled && !ExtentScale.Enabled && !DivergenceScale.Enabled)
        {
            yield break;
        }

        var gainDb = (float)(GainDb.Enabled ? GainDb.Value : 0.0);
        var diffuse = (float)(DiffuseScale.Enabled ? DiffuseScale.Value : 1.0);
        var extent = (float)(ExtentScale.Enabled ? ExtentScale.Value : 1.0);
        var divergence = (float)(DivergenceScale.Enabled ? DivergenceScale.Value : 1.0);
        foreach (var m in Members)
        {
            yield return new MonitorOverride(m.Id, gainDb, diffuse, extent, divergence);
        }
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
