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

    private void OnRowChanged() => RebuildPolicy();

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
