using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace MacinRender.Gui.Controls;

/// <summary>
/// 用 figma 风格 corner-smoothing(逆向 iOS/Apple continuous corner)的容器。
/// Avalonia 内置 CornerRadius 是标准圆弧(circular)、Border.Render 为 sealed 无法改,
/// 故继承 Decorator 自绘:补 Background / BorderBrush / BorderThickness / CornerRadius / Smoothing。
/// </summary>
public class SquircleBorder : Decorator
{
    public static readonly StyledProperty<IBrush?> BackgroundProperty =
        AvaloniaProperty.Register<SquircleBorder, IBrush?>(nameof(Background));

    public static readonly StyledProperty<IBrush?> BorderBrushProperty =
        AvaloniaProperty.Register<SquircleBorder, IBrush?>(nameof(BorderBrush));

    public static readonly StyledProperty<double> BorderThicknessProperty =
        AvaloniaProperty.Register<SquircleBorder, double>(nameof(BorderThickness));

    public static readonly StyledProperty<double> CornerRadiusProperty =
        AvaloniaProperty.Register<SquircleBorder, double>(nameof(CornerRadius), 18.0);

    /// <summary>0 = 普通圆角;0.6 ≈ iOS;1.0 = 最平滑。</summary>
    public static readonly StyledProperty<double> SmoothingProperty =
        AvaloniaProperty.Register<SquircleBorder, double>(nameof(Smoothing), 1.0);

    static SquircleBorder()
    {
        AffectsRender<SquircleBorder>(BackgroundProperty, BorderBrushProperty,
            BorderThicknessProperty, CornerRadiusProperty, SmoothingProperty);
    }

    public IBrush? Background
    {
        get => GetValue(BackgroundProperty);
        set => SetValue(BackgroundProperty, value);
    }

    public IBrush? BorderBrush
    {
        get => GetValue(BorderBrushProperty);
        set => SetValue(BorderBrushProperty, value);
    }

    public double BorderThickness
    {
        get => GetValue(BorderThicknessProperty);
        set => SetValue(BorderThicknessProperty, value);
    }

    public double CornerRadius
    {
        get => GetValue(CornerRadiusProperty);
        set => SetValue(CornerRadiusProperty, value);
    }

    public double Smoothing
    {
        get => GetValue(SmoothingProperty);
        set => SetValue(SmoothingProperty, value);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var size = base.ArrangeOverride(finalSize);
        Clip = SquircleGeometry.Create(new Rect(size), CornerRadius, Smoothing);
        return size;
    }

    public override void Render(DrawingContext context)
    {
        double bt = BorderThickness;
        var rect = new Rect(Bounds.Size).Deflate(new Thickness(bt / 2));
        var geo = SquircleGeometry.Create(rect, CornerRadius, Smoothing);
        IPen? pen = (bt > 0 && BorderBrush is not null) ? new Pen(BorderBrush, bt) : null;
        context.DrawGeometry(Background, pen, geo);
    }
}

internal static class SquircleGeometry
{
    private static double Rad(double deg) => deg * Math.PI / 180.0;

    /// <summary>figma corner smoothing:圆角由 cubic→arc→cubic 组成,平滑区沿直边延展。</summary>
    public static StreamGeometry Create(Rect rect, double radius, double smoothing = 1.0)
    {
        double w = rect.Width, h = rect.Height, ox = rect.X, oy = rect.Y;
        var geo = new StreamGeometry();
        var ctx = geo.Open();

        double r = Math.Max(0, Math.Min(radius, Math.Min(w, h) / 2));
        if (r <= 0.01)
        {
            ctx.BeginFigure(new Point(ox, oy), true);
            ctx.LineTo(new Point(ox + w, oy));
            ctx.LineTo(new Point(ox + w, oy + h));
            ctx.LineTo(new Point(ox, oy + h));
            ctx.EndFigure(true);
            ctx.Dispose();
            return geo;
        }

        double budget = Math.Min(w, h) / 2;
        double s = Math.Clamp(smoothing, 0, Math.Max(0, budget / r - 1));
        double p = Math.Min((1 + s) * r, budget);

        double arcMeasure = 90 * (1 - s);
        double arcSection = Math.Sin(Rad(arcMeasure / 2)) * r * Math.Sqrt(2);
        double angleAlpha = (90 - arcMeasure) / 2;
        double p3p4 = r * Math.Tan(Rad(angleAlpha / 2));
        double angleBeta = 45 * s;
        double cc = p3p4 * Math.Cos(Rad(angleBeta));
        double dd = cc * Math.Tan(Rad(angleBeta));
        double b = (p - arcSection - cc - dd) / 3;
        double a = 2 * b;

        var cur = new Point(ox + w - p, oy);
        ctx.BeginFigure(cur, true);

        Point Rel(double dx, double dy) => new Point(cur.X + dx, cur.Y + dy);

        // 顺时针:右上 → 右下 → 左下 → 左上
        // 右上角
        ctx.CubicBezierTo(Rel(a, 0), Rel(a + b, 0), Rel(a + b + cc, dd));
        cur = Rel(a + b + cc, dd);
        ctx.ArcTo(Rel(arcSection, arcSection), new Size(r, r), 0, false, SweepDirection.Clockwise);
        cur = Rel(arcSection, arcSection);
        ctx.CubicBezierTo(Rel(dd, cc), Rel(dd, b + cc), Rel(dd, a + b + cc));
        cur = Rel(dd, a + b + cc);

        cur = new Point(ox + w, oy + h - p);
        ctx.LineTo(cur);

        // 右下角
        ctx.CubicBezierTo(Rel(0, a), Rel(0, a + b), Rel(-dd, a + b + cc));
        cur = Rel(-dd, a + b + cc);
        ctx.ArcTo(Rel(-arcSection, arcSection), new Size(r, r), 0, false, SweepDirection.Clockwise);
        cur = Rel(-arcSection, arcSection);
        ctx.CubicBezierTo(Rel(-cc, dd), Rel(-(b + cc), dd), Rel(-(a + b + cc), dd));
        cur = Rel(-(a + b + cc), dd);

        cur = new Point(ox + p, oy + h);
        ctx.LineTo(cur);

        // 左下角
        ctx.CubicBezierTo(Rel(-a, 0), Rel(-(a + b), 0), Rel(-(a + b + cc), -dd));
        cur = Rel(-(a + b + cc), -dd);
        ctx.ArcTo(Rel(-arcSection, -arcSection), new Size(r, r), 0, false, SweepDirection.Clockwise);
        cur = Rel(-arcSection, -arcSection);
        ctx.CubicBezierTo(Rel(-dd, -cc), Rel(-dd, -(b + cc)), Rel(-dd, -(a + b + cc)));
        cur = Rel(-dd, -(a + b + cc));

        cur = new Point(ox, oy + p);
        ctx.LineTo(cur);

        // 左上角
        ctx.CubicBezierTo(Rel(0, -a), Rel(0, -(a + b)), Rel(dd, -(a + b + cc)));
        cur = Rel(dd, -(a + b + cc));
        ctx.ArcTo(Rel(arcSection, -arcSection), new Size(r, r), 0, false, SweepDirection.Clockwise);
        cur = Rel(arcSection, -arcSection);
        ctx.CubicBezierTo(Rel(cc, -dd), Rel(b + cc, -dd), Rel(a + b + cc, -dd));
        cur = Rel(a + b + cc, -dd);

        ctx.EndFigure(true);
        ctx.Dispose();
        return geo;
    }
}
