using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using MacinRender.Gui.Services;

namespace MacinRender.Gui.Views;

// 伪 3D 空间视图:ADM 归一化立方体(线框)+ 中心 Minecraft 风格人头(为头追踪预留独立朝向)
// + 对象点 / 运动轨迹,随播放头动画。自绘(DrawingContext),无 3D 引擎。
//
// 投影:模型点先经"人头朝向"(仅作用于人头局部点)再经"相机轨道旋转(yaw/pitch)",
// 正交投影到屏幕;深度 = 旋转后 Y(越大越远)。相机与人头朝向解耦 → 头追踪只需设 HeadRotation。
internal sealed class SpatialSceneControl : Control
{
    public static readonly StyledProperty<SpatialScene?> SceneProperty =
        AvaloniaProperty.Register<SpatialSceneControl, SpatialScene?>(nameof(Scene));

    public static readonly StyledProperty<double> CurrentTimeProperty =
        AvaloniaProperty.Register<SpatialSceneControl, double>(nameof(CurrentTime));

    public static readonly StyledProperty<bool> ShowLabelsProperty =
        AvaloniaProperty.Register<SpatialSceneControl, bool>(nameof(ShowLabels), true);

    // false = 拖尾(只画播放头前一小段,淡出);true = 累积(轨迹经过即保留,不消失)。
    public static readonly StyledProperty<bool> PersistTrailProperty =
        AvaloniaProperty.Register<SpatialSceneControl, bool>(nameof(PersistTrail));

    public SpatialScene? Scene
    {
        get => GetValue(SceneProperty);
        set => SetValue(SceneProperty, value);
    }

    public double CurrentTime
    {
        get => GetValue(CurrentTimeProperty);
        set => SetValue(CurrentTimeProperty, value);
    }

    public bool ShowLabels
    {
        get => GetValue(ShowLabelsProperty);
        set => SetValue(ShowLabelsProperty, value);
    }

    public bool PersistTrail
    {
        get => GetValue(PersistTrailProperty);
        set => SetValue(PersistTrailProperty, value);
    }

    // 相机轨道角(弧度):默认 3/4 俯视。
    private double _yaw = -0.45;
    private double _pitch = 0.35;
    private double _zoom = 1.0;
    private Point _lastPointer;
    private bool _dragging;

    // 人头朝向(yaw/pitch/roll,弧度)。v1 恒等;未来头追踪写入即转。
    private double _headYaw;
    private double _headPitch;
    private double _headRoll;

    static SpatialSceneControl()
    {
        AffectsRender<SpatialSceneControl>(SceneProperty, CurrentTimeProperty, ShowLabelsProperty,
            PersistTrailProperty);
    }

    public void SetHeadOrientation(double yaw, double pitch, double roll)
    {
        _headYaw = yaw;
        _headPitch = pitch;
        _headRoll = roll;
        InvalidateVisual();
    }

    public void ResetView()
    {
        _yaw = -0.45;
        _pitch = 0.35;
        _zoom = 1.0;
        InvalidateVisual();
    }

    // ── 交互:拖拽轨道旋转、滚轮缩放、双击复位 ──
    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (e.ClickCount == 2)
        {
            ResetView();
            return;
        }

        _lastPointer = e.GetPosition(this);
        _dragging = true;
        e.Pointer.Capture(this);
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (!_dragging)
        {
            return;
        }

        var p = e.GetPosition(this);
        _yaw += (p.X - _lastPointer.X) * 0.01;
        _pitch += (p.Y - _lastPointer.Y) * 0.01;
        _pitch = Math.Clamp(_pitch, -1.45, 1.45);
        _lastPointer = p;
        InvalidateVisual();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        _dragging = false;
        e.Pointer.Capture(null);
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        _zoom = Math.Clamp(_zoom * (1.0 + (e.Delta.Y * 0.1)), 0.4, 3.0);
        InvalidateVisual();
    }

    // ── 投影 ──
    private Vec3 RotateCamera(Vec3 p)
    {
        double cy = Math.Cos(_yaw);
        double sy = Math.Sin(_yaw);
        double x1 = (p.X * cy) - (p.Y * sy);
        double y1 = (p.X * sy) + (p.Y * cy);
        double z1 = p.Z;
        double cp = Math.Cos(_pitch);
        double sp = Math.Sin(_pitch);
        double y2 = (y1 * cp) - (z1 * sp);
        double z2 = (y1 * sp) + (z1 * cp);
        return new Vec3(x1, y2, z2);
    }

    private Vec3 RotateHead(Vec3 p)
    {
        // roll(绕 Y)→ pitch(绕 X)→ yaw(绕 Z)
        double cr = Math.Cos(_headRoll);
        double sr = Math.Sin(_headRoll);
        double x1 = (p.X * cr) + (p.Z * sr);
        double z1 = (-p.X * sr) + (p.Z * cr);
        double y1 = p.Y;
        double cp = Math.Cos(_headPitch);
        double sp = Math.Sin(_headPitch);
        double y2 = (y1 * cp) - (z1 * sp);
        double z2 = (y1 * sp) + (z1 * cp);
        double cy = Math.Cos(_headYaw);
        double sy = Math.Sin(_headYaw);
        double x3 = (x1 * cy) - (y2 * sy);
        double y3 = (x1 * sy) + (y2 * cy);
        return new Vec3(x3, y3, z2);
    }

    private double _cx;
    private double _cy;
    private double _scale;

    private Point Project(Vec3 v) => new(_cx + (_scale * v.X), _cy - (_scale * v.Z));

    public override void Render(DrawingContext context)
    {
        var b = Bounds;
        context.FillRectangle(s_background, new Rect(0, 0, b.Width, b.Height));

        _cx = b.Width / 2.0;
        _cy = b.Height / 2.0;
        _scale = Math.Min(b.Width, b.Height) / 2.0 * 0.78 * _zoom;

        DrawCube(context);
        DrawHeadAndObjects(context);
    }

    private void DrawCube(DrawingContext context)
    {
        // 8 角点 (±1,±1,±1) 的 12 条棱。
        var c = new Vec3[8];
        int idx = 0;
        for (int sx = -1; sx <= 1; sx += 2)
        {
            for (int sy = -1; sy <= 1; sy += 2)
            {
                for (int sz = -1; sz <= 1; sz += 2)
                {
                    c[idx++] = RotateCamera(new Vec3(sx, sy, sz));
                }
            }
        }

        // 角点按 (x,y,z) 位序:bit2=x,bit1=y,bit0=z。相邻仅一位不同 = 一条棱。
        for (int i = 0; i < 8; i++)
        {
            for (int bit = 1; bit <= 4; bit <<= 1)
            {
                int j = i ^ bit;
                if (j > i)
                {
                    context.DrawLine(s_cubePen, Project(c[i]), Project(c[j]));
                }
            }
        }
    }

    // 头立方体一个面:UV 序角点 c00(左上) c10(右上) c11(右下) c01(左下) + 法线 + 底色 + 是否前脸。
    private readonly struct Face
    {
        public readonly Vec3 C00;
        public readonly Vec3 C10;
        public readonly Vec3 C11;
        public readonly Vec3 C01;
        public readonly Vec3 Normal;
        public readonly Color Base;
        public readonly bool IsFront;

        public Face(Vec3 c00, Vec3 c10, Vec3 c11, Vec3 c01, Vec3 normal, Color baseColor, bool isFront)
        {
            C00 = c00;
            C10 = c10;
            C11 = c11;
            C01 = c01;
            Normal = normal;
            Base = baseColor;
            IsFront = isFront;
        }

        public Vec3 Uv(double u, double v)
        {
            Vec3 top = Lerp(C00, C10, u);
            Vec3 bot = Lerp(C01, C11, u);
            return Lerp(top, bot, v);
        }

        private static Vec3 Lerp(Vec3 a, Vec3 b, double t) => Vec3.Lerp(a, b, t);
    }

    private void DrawHeadAndObjects(DrawingContext context)
    {
        var scene = Scene;

        // 对象当前状态 + 深度,按相对人头(深度 0)前后两批,实现人头遮挡。
        var samples = new List<(SpatialObject Obj, SpatialSample S, Vec3 View)>();
        if (scene is not null)
        {
            foreach (var obj in scene.Objects)
            {
                if (obj.IsBed)
                {
                    continue;
                }

                var s = obj.SampleAt(CurrentTime);
                samples.Add((obj, s, RotateCamera(s.Position)));
            }
        }

        // 远批(深度 > 0,在人头后)先画。
        foreach (var item in samples)
        {
            if (item.View.Y > 0)
            {
                DrawObjectDot(context, item.Obj, item.S, item.View);
            }
        }

        DrawTrajectories(context, scene);
        DrawBeds(context, scene);
        DrawHead(context);

        // 近批(深度 <= 0,在人头前)后画。
        foreach (var item in samples)
        {
            if (item.View.Y <= 0)
            {
                DrawObjectDot(context, item.Obj, item.S, item.View);
            }
        }
    }

    private const double HeadHalf = 0.18;

    private void DrawHead(DrawingContext context)
    {
        const double h = HeadHalf;
        var faces = new[]
        {
            // 前脸(+Y,肤色):u→+X,v→-Z
            new Face(new Vec3(-h, h, h), new Vec3(h, h, h), new Vec3(h, h, -h), new Vec3(-h, h, -h),
                new Vec3(0, 1, 0), s_skin, true),
            // 后脑(-Y,发色)
            new Face(new Vec3(h, -h, h), new Vec3(-h, -h, h), new Vec3(-h, -h, -h), new Vec3(h, -h, -h),
                new Vec3(0, -1, 0), s_hair, false),
            // 右(+X,发色)
            new Face(new Vec3(h, h, h), new Vec3(h, -h, h), new Vec3(h, -h, -h), new Vec3(h, h, -h),
                new Vec3(1, 0, 0), s_hair, false),
            // 左(-X,发色)
            new Face(new Vec3(-h, -h, h), new Vec3(-h, h, h), new Vec3(-h, h, -h), new Vec3(-h, -h, -h),
                new Vec3(-1, 0, 0), s_hair, false),
            // 顶(+Z,发色)
            new Face(new Vec3(-h, -h, h), new Vec3(h, -h, h), new Vec3(h, h, h), new Vec3(-h, h, h),
                new Vec3(0, 0, 1), s_hair, false),
            // 底(-Z,肤色:脖子)
            new Face(new Vec3(-h, h, -h), new Vec3(h, h, -h), new Vec3(h, -h, -h), new Vec3(-h, -h, -h),
                new Vec3(0, 0, -1), s_skin, false),
        };

        // 面心深度(经人头朝向 + 相机),从后往前画(画家算法 → 背面自然被覆盖)。
        var order = new int[faces.Length];
        var depth = new double[faces.Length];
        for (int i = 0; i < faces.Length; i++)
        {
            order[i] = i;
            var center = faces[i].Uv(0.5, 0.5);
            depth[i] = RotateCamera(RotateHead(center)).Y;
        }

        Array.Sort(depth, order);
        Array.Reverse(order); // 远→近

        foreach (int fi in order)
        {
            var f = faces[fi];
            double shade = FaceShade(f.Normal);
            FillFaceRect(context, f, 0, 0, 1, 1, Shade(f.Base, shade));
            if (f.IsFront)
            {
                DrawSteveFace(context, f, shade);
            }
        }
    }

    // 前脸像素五官(Steve 风格),UV 0..1,v=0 顶。
    private void DrawSteveFace(DrawingContext context, Face f, double shade)
    {
        Color hair = Shade(s_hair, shade);
        Color white = Shade(s_eyeWhite, shade);
        Color iris = Shade(s_iris, shade);
        Color skinDark = Shade(s_skinDark, shade);
        Color mouth = Shade(s_mouth, shade);

        FillFaceRect(context, f, 0.0, 0.0, 1.0, 0.18, hair);     // 刘海
        FillFaceRect(context, f, 0.19, 0.30, 0.44, 0.36, hair);  // 左眉
        FillFaceRect(context, f, 0.56, 0.30, 0.81, 0.36, hair);  // 右眉
        FillFaceRect(context, f, 0.19, 0.37, 0.44, 0.50, white); // 左眼白
        FillFaceRect(context, f, 0.56, 0.37, 0.81, 0.50, white); // 右眼白
        FillFaceRect(context, f, 0.31, 0.37, 0.44, 0.50, iris);  // 左瞳(内侧)
        FillFaceRect(context, f, 0.56, 0.37, 0.69, 0.50, iris);  // 右瞳(内侧)
        FillFaceRect(context, f, 0.44, 0.50, 0.56, 0.61, skinDark); // 鼻
        FillFaceRect(context, f, 0.31, 0.66, 0.69, 0.72, mouth);    // 嘴
    }

    private void FillFaceRect(DrawingContext context, Face f, double u0, double v0, double u1, double v1, Color color)
    {
        Point p00 = Project(RotateCamera(RotateHead(f.Uv(u0, v0))));
        Point p10 = Project(RotateCamera(RotateHead(f.Uv(u1, v0))));
        Point p11 = Project(RotateCamera(RotateHead(f.Uv(u1, v1))));
        Point p01 = Project(RotateCamera(RotateHead(f.Uv(u0, v1))));

        var geo = new StreamGeometry();
        using (var gc = geo.Open())
        {
            gc.BeginFigure(p00, true);
            gc.LineTo(p10);
            gc.LineTo(p11);
            gc.LineTo(p01);
            gc.EndFigure(true);
        }

        context.DrawGeometry(new SolidColorBrush(color), null, geo);
    }

    private double FaceShade(Vec3 normalLocal)
    {
        var n = RotateCamera(RotateHead(normalLocal));
        // 光源:左上前方。归一化法线后取 n·L,映射到 [0.55, 1.0]。
        double len = Math.Sqrt((n.X * n.X) + (n.Y * n.Y) + (n.Z * n.Z));
        if (len < 1e-9)
        {
            return 1.0;
        }

        double dot = ((n.X * -0.35) + (n.Y * -0.5) + (n.Z * 0.79)) / len;
        return 0.55 + (0.45 * Math.Clamp((dot + 1.0) / 2.0, 0.0, 1.0));
    }

    // 拖尾时长(秒):只画播放头之前这段窗口的运动轨迹,越接近"现在"越亮越粗 → 彗尾。
    private const double TrailSeconds = 2.5;

    private void DrawTrajectories(DrawingContext context, SpatialScene? scene)
    {
        if (scene is null)
        {
            return;
        }

        double now = CurrentTime;
        bool persist = PersistTrail;
        foreach (var obj in scene.Objects)
        {
            if (obj.IsBed || obj.Keyframes.Count < 2)
            {
                continue;
            }

            Color baseCol = ObjectColor(obj.ColorIndex);
            if (persist)
            {
                DrawPersistTrail(context, obj, now, baseCol);
            }
            else
            {
                DrawFadeTrail(context, obj, now, baseCol);
            }
        }
    }

    // 拖尾:只画播放头前 TrailSeconds 这段滑动窗口,越接近"现在"越亮越粗(彗尾)。
    // 顶点落在真实轨迹拐点(各 block 插值起点/终点这些固定绝对时刻)上 → 中间不抽动,
    // 只有窗口两端 t0/now 随播放头沿当前直线段平滑平移。
    private void DrawFadeTrail(DrawingContext context, SpatialObject obj, double now, Color col)
    {
        double t0 = Math.Max(0.0, now - TrailSeconds);
        double span = now - t0;
        if (span < 1e-4)
        {
            return;
        }

        // 收集窗口内真实轨迹的时间断点,加上两端,排序。
        var times = new List<double> { t0 };
        foreach (var kf in obj.Keyframes)
        {
            double a = kf.TimeSeconds;
            double b = kf.TimeSeconds + kf.InterpSeconds;
            if (a > t0 && a < now)
            {
                times.Add(a);
            }

            if (b > t0 && b < now)
            {
                times.Add(b);
            }
        }

        times.Add(now);
        times.Sort();

        Point prev = Project(RotateCamera(obj.SampleAt(times[0]).Position));
        for (int i = 1; i < times.Count; i++)
        {
            var smp = obj.SampleAt(times[i]);
            Point cur = Project(RotateCamera(smp.Position));
            double f = (times[i] - t0) / span; // 0 = 尾(旧)→ 1 = 头(现在)
            double alpha = 0.5 * f * (smp.Active ? 1.0 : 0.35);
            double w = 0.8 + (2.0 * f);
            context.DrawLine(new Pen(new SolidColorBrush(WithAlpha(col, alpha)), w), prev, cur);
            prev = cur;
        }
    }

    // 累积:连接固定的轨迹拐点(每个已开始 block 的目标位置 = keyframe 顶点)直到播放头,
    // 末端在插值中途时收到当前实时位置。顶点不随播放头变 → 真"足迹",不抽动。
    private void DrawPersistTrail(DrawingContext context, SpatialObject obj, double now, Color col)
    {
        var kfs = obj.Keyframes;
        if (kfs[0].TimeSeconds > now)
        {
            return; // 对象尚未开始
        }

        var pen = new Pen(new SolidColorBrush(WithAlpha(col, 0.42)), 1.4);
        Point prev = Project(RotateCamera(kfs[0].Position));
        for (int i = 1; i < kfs.Count; i++)
        {
            var kf = kfs[i];
            if (kf.TimeSeconds > now)
            {
                break; // 该 block 还没开始
            }

            // block 已到达目标 → 用 keyframe 顶点(固定);仍在插值中 → 用当前实时位置并收尾。
            bool arrived = kf.TimeSeconds + kf.InterpSeconds <= now;
            Vec3 target = arrived ? kf.Position : obj.SampleAt(now).Position;
            Point cur = Project(RotateCamera(target));
            context.DrawLine(pen, prev, cur);
            prev = cur;
            if (!arrived)
            {
                break;
            }
        }
    }

    private void DrawObjectDot(DrawingContext context, SpatialObject obj, SpatialSample s, Vec3 view)
    {
        Point p = Project(view);
        Color col = ObjectColor(obj.ColorIndex);
        double extent = (s.Width + s.Height + s.Depth) / 3.0;
        double glowR = 6.0 + (extent * _scale * 0.5);
        double coreR = 5.0;

        double op = s.Active ? 1.0 : 0.22;
        // extent 光晕
        context.DrawEllipse(new SolidColorBrush(WithAlpha(col, 0.18 * op)), null, p, glowR, glowR);
        // 核心点(透明度随增益)
        double gainOp = op * Math.Clamp(s.Gain, 0.15, 1.0);
        context.DrawEllipse(new SolidColorBrush(WithAlpha(col, gainOp)),
            new Pen(new SolidColorBrush(WithAlpha(Colors.White, 0.6 * op)), 1.0), p, coreR, coreR);

        if (ShowLabels && s.Active && !string.IsNullOrEmpty(obj.Name))
        {
            var ft = new FormattedText(obj.Name, CultureInfo.CurrentCulture, FlowDirection.LeftToRight,
                Typeface.Default, 11, new SolidColorBrush(WithAlpha(Colors.White, 0.85)));
            context.DrawText(ft, new Point(p.X + glowR + 3, p.Y - 7));
        }
    }

    private void DrawBeds(DrawingContext context, SpatialScene? scene)
    {
        if (scene is null)
        {
            return;
        }

        foreach (var obj in scene.Objects)
        {
            if (!obj.IsBed)
            {
                continue;
            }

            Color col = WithAlpha(ObjectColor(obj.ColorIndex), 0.55);
            foreach (var bp in obj.BedPoints)
            {
                Point p = Project(RotateCamera(bp.Position));
                var rect = new Rect(p.X - 3, p.Y - 3, 6, 6);
                context.FillRectangle(new SolidColorBrush(col), rect);
            }
        }
    }

    // ── 配色 / 工具 ──
    private static readonly IBrush s_background = new SolidColorBrush(Color.FromRgb(0x16, 0x18, 0x1C));
    private static readonly IPen s_cubePen = new Pen(new SolidColorBrush(Color.FromArgb(0x55, 0xAE, 0xAE, 0xB2)), 1.0);

    private static readonly Color s_skin = Color.FromRgb(0xBB, 0x8B, 0x61);
    private static readonly Color s_skinDark = Color.FromRgb(0x9C, 0x70, 0x4C);
    private static readonly Color s_hair = Color.FromRgb(0x45, 0x30, 0x1E);
    private static readonly Color s_eyeWhite = Color.FromRgb(0xE6, 0xE6, 0xE6);
    private static readonly Color s_iris = Color.FromRgb(0x4A, 0x5B, 0x8C);
    private static readonly Color s_mouth = Color.FromRgb(0x6B, 0x4A, 0x38);

    private static readonly Color[] s_palette =
    {
        Color.FromRgb(0xFF, 0x5E, 0x5B), Color.FromRgb(0xFF, 0xB4, 0x00), Color.FromRgb(0x36, 0xC5, 0xF0),
        Color.FromRgb(0x2E, 0xB6, 0x7D), Color.FromRgb(0xA6, 0x6B, 0xFF), Color.FromRgb(0xFF, 0x7A, 0xC6),
        Color.FromRgb(0x00, 0xC2, 0xA8), Color.FromRgb(0xF0, 0x9E, 0x36),
    };

    private static Color ObjectColor(int i) => s_palette[((i % s_palette.Length) + s_palette.Length) % s_palette.Length];

    private static Color WithAlpha(Color c, double a) =>
        Color.FromArgb((byte)Math.Clamp(a * 255.0, 0, 255), c.R, c.G, c.B);

    private static Color Shade(Color c, double k) => Color.FromArgb(c.A,
        (byte)Math.Clamp(c.R * k, 0, 255), (byte)Math.Clamp(c.G * k, 0, 255), (byte)Math.Clamp(c.B * k, 0, 255));
}
