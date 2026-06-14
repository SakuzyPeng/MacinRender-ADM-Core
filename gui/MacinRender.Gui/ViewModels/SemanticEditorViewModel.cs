using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.ViewModels;

/// <summary>
/// 语义编辑模式 ViewModel(单文档工作台)。载入单个 ADM → adm_inspect_file_json 列出对象 +
/// 各听感维度的"当前值"(逐块实际值,区间感知)。编辑为"相对变换"(× scale / dB / 静音),
/// 内存拼 mradm.semantic-policy.v1 JSON 供后续试听/渲染消费(#16/#17/#18)。
/// </summary>
public sealed partial class SemanticEditorViewModel : ObservableObject
{
    private static Localizer L => Localizer.Instance;

    public ObservableCollection<SemanticObjectItem> Objects { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasFile))]
    private string? _loadedPath;

    [ObservableProperty] private SemanticObjectItem? _selectedObject;
    [ObservableProperty] private string _statusText = "";
    [ObservableProperty] private bool _isLoading;

    /// <summary>当前 policy JSON(无任何覆盖时为空串);供 #16/#17/#18 消费。</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    private string _policyJson = "";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(OverrideSummary))]
    private int _overriddenObjectCount;

    public bool HasFile => !string.IsNullOrEmpty(LoadedPath);

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
        };
    }

    public async Task LoadFileAsync(string path)
    {
        if (string.IsNullOrEmpty(path) || IsLoading)
        {
            return;
        }

        IsLoading = true;
        StatusText = L.Format("SemLoading", Path.GetFileName(path));
        try
        {
            var doc = await Task.Run(() =>
            {
                using var ctx = NativeMethods.adm_create_context();
                return ctx.IsInvalid ? null : SemanticInspect.Parse(AdmQueries.FetchInspectJson(ctx, path));
            });

            foreach (var existing in Objects)
            {
                existing.Changed -= OnObjectChanged;
            }

            Objects.Clear();
            SelectedObject = null;
            if (doc is null)
            {
                LoadedPath = null;
                StatusText = L["SemLoadFailed"];
                RebuildPolicy();
                return;
            }

            foreach (var obj in doc.Objects)
            {
                var item = SemanticObjectItem.From(obj);
                item.Changed += OnObjectChanged;
                Objects.Add(item);
            }

            LoadedPath = path;
            SelectedObject = Objects.Count > 0 ? Objects[0] : null;
            StatusText = L.Format("SemLoaded", Objects.Count);
            RebuildPolicy();
        }
        finally
        {
            IsLoading = false;
        }
    }

    private void OnObjectChanged() => RebuildPolicy();

    // 任一对象的覆盖变化 → 重拼整份 policy JSON(每个有覆盖的对象一条 id 规则)。
    // 用 List<JsonNode?> + JsonArray 构造器(避开 JsonArray.Add<T> 的 AOT/trim 装箱告警)。
    private void RebuildPolicy()
    {
        var rules = new List<JsonNode?>();
        foreach (var item in Objects)
        {
            if (item.BuildRule() is { } rule)
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
}

/// <summary>min–max 区间;Max−Min 超阈视为"动态"(逐块变化),否则为常量单值。</summary>
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
/// 一个标量"覆盖行":[☐ 覆盖] + 值控件。值是相对变换——ScaleLinear 为逐块 × 系数(钳到 [lo,hi]);
/// GainDb 为 dB 偏移(对象级单值)。CurrentText/EffectiveText 即三栏的"当前值"与客户端即时"生效值"。
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
    private double _value;

    partial void OnEnabledChanged(bool value) => Changed?.Invoke();
    partial void OnValueChanged(double value) => Changed?.Invoke();

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

    /// <summary>启用时写出该维度的 policy 片段(scale 或 gain_db);未启用返回 null。</summary>
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
/// 场景树中的一个对象:身份 + 听感维度当前值(区间感知)+ 可编辑的相对覆盖。
/// 当前值权威源是 adm_inspect_file_json 的逐块实际取值(扫全块求 min/max),非 policy_template_json
/// (后者是中性骨架,不含实际值)。
/// </summary>
public sealed class SemanticObjectItem
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    public int? Importance { get; init; }
    public int? DialogueId { get; init; }
    public bool OriginalMute { get; init; }

    // 听感维度覆盖行(忠于 ADM 原词;gain 为 dB 偏移,其余为逐块 × scale)。
    public required ScalarOverride GainDb { get; init; }
    public required ScalarOverride DiffuseScale { get; init; }
    public required ScalarOverride ExtentScale { get; init; }
    public required ScalarOverride DivergenceScale { get; init; }

    public event Action? Changed;

    public string Display => string.IsNullOrEmpty(Name) ? Id : $"{Name}  ({Id})";

    public string Summary =>
        $"gain {GainDb.CurrentText} · diffuse {DiffuseScale.CurrentText} · " +
        $"extent {ExtentScale.CurrentText} · divergence {DivergenceScale.CurrentText}";

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

            if (OriginalMute)
            {
                parts.Add("mute");
            }

            return string.Join(" · ", parts);
        }
    }

    /// <summary>该对象的 policy 规则(无覆盖返回 null)。每维度片段 + id 匹配。</summary>
    public JsonObject? BuildRule()
    {
        var rule = new JsonObject();
        AddIf(rule, "gain", GainDb.ToPolicyFragment());
        AddIf(rule, "diffuse", DiffuseScale.ToPolicyFragment());
        AddIf(rule, "extent", ExtentScale.ToPolicyFragment());
        AddIf(rule, "divergence", DivergenceScale.ToPolicyFragment());
        if (rule.Count == 0)
        {
            return null;
        }

        rule["id"] = Id;
        return rule;
    }

    private static void AddIf(JsonObject rule, string key, JsonObject? fragment)
    {
        if (fragment is not null)
        {
            rule[key] = fragment;
        }
    }

    internal static SemanticObjectItem From(InspectObject obj)
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

        var item = new SemanticObjectItem
        {
            Id = obj.Id,
            Name = obj.Name,
            Importance = obj.Importance,
            DialogueId = obj.DialogueId,
            OriginalMute = obj.Mute,
            GainDb = new ScalarOverride("gain", ScalarOverride.Mode.GainDb,
                new[] { new ScalarOverride.Axis("", new DimRange(obj.Gain, obj.Gain)) }, 0.0, -24.0, 12.0),
            DiffuseScale = new ScalarOverride("diffuse", ScalarOverride.Mode.ScaleLinear,
                new[] { new ScalarOverride.Axis("", DimRange.Of(diffuse)) }, 1.0, 0.0, 4.0, 0.0, 1.0),
            ExtentScale = new ScalarOverride("extent", ScalarOverride.Mode.ScaleLinear,
                new[]
                {
                    new ScalarOverride.Axis("W", DimRange.Of(width)),
                    new ScalarOverride.Axis("H", DimRange.Of(height)),
                    new ScalarOverride.Axis("D", DimRange.Of(depth)),
                }, 1.0, 0.0, 4.0),
            DivergenceScale = new ScalarOverride("divergence", ScalarOverride.Mode.ScaleLinear,
                new[] { new ScalarOverride.Axis("", DimRange.Of(divergence)) }, 1.0, 0.0, 4.0, 0.0, 1.0),
        };

        foreach (var ov in new[] { item.GainDb, item.DiffuseScale, item.ExtentScale, item.DivergenceScale })
        {
            ov.Changed += () => item.Changed?.Invoke();
        }

        return item;
    }
}
