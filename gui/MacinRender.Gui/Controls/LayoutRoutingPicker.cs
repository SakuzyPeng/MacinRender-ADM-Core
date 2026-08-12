using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Data;
using Avalonia.Layout;
using Avalonia.Media;
using MacinRender.Gui.I18n;
using MacinRender.Gui.Interop;
using MacinRender.Gui.Models;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.Controls;

/// <summary>
/// 布局选择器。22.2 是唯一带二级菜单的布局，二级选 direct / split-power。
/// 收起后只显示布局名；split-power 通过 22.2 本身的强调色表意，不占用常驻文字。
/// </summary>
public sealed class LayoutRoutingPicker : ContentControl
{
    private sealed class PickerButton : SquircleButton
    {
        public Action? PrepareFlyout { get; init; }

        protected override void OpenFlyout()
        {
            PrepareFlyout?.Invoke();
            base.OpenFlyout();
        }
    }

    public static readonly StyledProperty<IEnumerable<LayoutDef>?> ItemsSourceProperty =
        AvaloniaProperty.Register<LayoutRoutingPicker, IEnumerable<LayoutDef>?>(nameof(ItemsSource));

    public static readonly StyledProperty<LayoutDef?> SelectedLayoutProperty =
        AvaloniaProperty.Register<LayoutRoutingPicker, LayoutDef?>(nameof(SelectedLayout),
            defaultBindingMode: BindingMode.TwoWay);

    public static readonly StyledProperty<AdmLfeRoutingMode> LfeRoutingModeProperty =
        AvaloniaProperty.Register<LayoutRoutingPicker, AdmLfeRoutingMode>(nameof(LfeRoutingMode),
            AdmLfeRoutingMode.Direct, defaultBindingMode: BindingMode.TwoWay);

    private readonly SquircleButton _button;
    private readonly TextBlock _layoutText;
    private readonly MenuFlyout _flyout;

    public LayoutRoutingPicker()
    {
        _layoutText = new TextBlock
        {
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        _layoutText.Classes.Add("layoutvalue");

        var arrow = new Path
        {
            Data = StreamGeometry.Parse("M4 8l6 6 6-6"),
            Width = 11,
            Height = 7,
            Margin = new Thickness(10, 0, 0, 0),
            Stretch = Stretch.Uniform,
            VerticalAlignment = VerticalAlignment.Center,
        };
        arrow.Classes.Add("layoutarrow");

        var content = new Grid();
        content.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Star));
        content.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        content.Children.Add(_layoutText);
        content.Children.Add(arrow);
        Grid.SetColumn(arrow, 1);

        _flyout = new MenuFlyout { Placement = PlacementMode.BottomEdgeAlignedLeft };

        _button = new PickerButton
        {
            Content = content,
            Flyout = _flyout,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            PrepareFlyout = RebuildMenu,
        };
        _button.Classes.Add("layoutpicker");
        _button.PointerEntered += (_, _) => RefreshTooltip();

        Content = _button;
        RefreshAppearance();
    }

    public IEnumerable<LayoutDef>? ItemsSource
    {
        get => GetValue(ItemsSourceProperty);
        set => SetValue(ItemsSourceProperty, value);
    }

    public LayoutDef? SelectedLayout
    {
        get => GetValue(SelectedLayoutProperty);
        set => SetValue(SelectedLayoutProperty, value);
    }

    public AdmLfeRoutingMode LfeRoutingMode
    {
        get => GetValue(LfeRoutingModeProperty);
        set => SetValue(LfeRoutingModeProperty, value);
    }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == SelectedLayoutProperty || change.Property == LfeRoutingModeProperty)
        {
            RefreshAppearance();
        }
    }

    private bool IsSelected(LayoutDef layout) =>
        string.Equals(SelectedLayout?.Id, layout.Id, StringComparison.OrdinalIgnoreCase);

    private void RebuildMenu()
    {
        _flyout.Items.Clear();
        foreach (var layout in ItemsSource ?? Array.Empty<LayoutDef>())
        {
            if (RenderSettings.Is22Point2Layout(layout.Id))
            {
                _flyout.Items.Add(Build22Point2Menu(layout));
                continue;
            }

            var item = new MenuItem
            {
                Header = layout.Name,
                ToggleType = MenuItemToggleType.Radio,
                GroupName = "output-layout",
                IsChecked = IsSelected(layout),
            };
            item.Click += (_, _) => SelectedLayout = layout;
            _flyout.Items.Add(item);
        }
    }

    private MenuItem Build22Point2Menu(LayoutDef layout)
    {
        var header = new TextBlock { Text = layout.Name };
        if (IsSelected(layout) && LfeRoutingMode == AdmLfeRoutingMode.SplitPower)
        {
            header.Classes.Add("lferoutingsplit");
        }

        var parent = new MenuItem { Header = header };
        parent.Icon = new TextBlock { Text = "✓", IsVisible = IsSelected(layout) };
        parent.Items.Add(BuildRoutingItem(layout, AdmLfeRoutingMode.Direct, "LfeRoutingDirect"));
        parent.Items.Add(BuildRoutingItem(layout, AdmLfeRoutingMode.SplitPower, "LfeRoutingSplitPower"));
        return parent;
    }

    private MenuItem BuildRoutingItem(LayoutDef layout, AdmLfeRoutingMode mode, string labelKey)
    {
        var item = new MenuItem
        {
            Header = Localizer.Instance[labelKey],
            ToggleType = MenuItemToggleType.Radio,
            GroupName = "lfe-routing",
            IsChecked = IsSelected(layout) && LfeRoutingMode == mode,
        };
        item.Click += (_, _) =>
        {
            // 先写模式再切布局，避免绑定链短暂保存上一个 22.2 偏好。
            LfeRoutingMode = mode;
            SelectedLayout = layout;
        };
        return item;
    }

    private void RefreshAppearance()
    {
        _layoutText.Text = SelectedLayout?.Name ?? string.Empty;
        var splitPower = RenderSettings.Is22Point2Layout(SelectedLayout?.Id) &&
                         LfeRoutingMode == AdmLfeRoutingMode.SplitPower;
        Classes.Set("split-power", splitPower);
        var accessibleName = _layoutText.Text;
        if (RenderSettings.Is22Point2Layout(SelectedLayout?.Id))
        {
            var modeKey = splitPower ? "LfeRoutingSplitPower" : "LfeRoutingDirect";
            accessibleName = $"{accessibleName}, LFE {Localizer.Instance[modeKey]}";
        }
        AutomationProperties.SetName(_button, accessibleName);
        RefreshTooltip();
    }

    private void RefreshTooltip()
    {
        if (!RenderSettings.Is22Point2Layout(SelectedLayout?.Id))
        {
            ToolTip.SetTip(_button, null);
            return;
        }

        var modeKey = LfeRoutingMode == AdmLfeRoutingMode.SplitPower
            ? "LfeRoutingSplitPower"
            : "LfeRoutingDirect";
        ToolTip.SetTip(_button, Localizer.Instance.Format("LfeRoutingTooltip", Localizer.Instance[modeKey]));
    }
}
