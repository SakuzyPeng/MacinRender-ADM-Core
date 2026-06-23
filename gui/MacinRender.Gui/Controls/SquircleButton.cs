using Avalonia;
using Avalonia.Controls;
using Avalonia.Rendering;

namespace MacinRender.Gui.Controls;

/// <summary>
/// 把按钮整体裁成 squircle 的 Button。配合样式里 PART_ContentPresenter 的 CornerRadius=0
/// (方角背景)使用,靠 Clip 得到平滑超椭圆外形。
/// </summary>
public class SquircleButton : Button, ICustomHitTest
{
    public static readonly StyledProperty<double> RadiusProperty =
        AvaloniaProperty.Register<SquircleButton, double>(nameof(Radius), 8.0);

    public static readonly StyledProperty<double> SmoothingProperty =
        AvaloniaProperty.Register<SquircleButton, double>(nameof(Smoothing), 1.0);

    public double Radius
    {
        get => GetValue(RadiusProperty);
        set => SetValue(RadiusProperty, value);
    }

    public double Smoothing
    {
        get => GetValue(SmoothingProperty);
        set => SetValue(SmoothingProperty, value);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var size = base.ArrangeOverride(finalSize);
        Clip = SquircleGeometry.Create(new Rect(size), Radius, Smoothing);
        return size;
    }

    public bool HitTest(Point point) => Clip?.FillContains(point) ?? new Rect(Bounds.Size).Contains(point);
}
