using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Immutable;
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

    // 每帧复用,避免重复分配(掉帧优化)。
    private readonly List<(SpatialObject Obj, SpatialSample S, Vec3 View)> _samples = new();
    private readonly List<(double T, bool JumpInto)> _trailPts = new();

    // 对象名 FormattedText 缓存(文本/字号/颜色恒定,只位置每帧变)。场景切换时清空。
    private readonly Dictionary<string, FormattedText> _labels = new();
    private SpatialScene? _lastScene;

    // 累积轨迹跨帧缓存:存"模型空间"结构(顶点不随相机变);几何仅在新增顶点或相机变化时重投影。
    // 单调播放且相机不动 → 每帧 O(1);旋转 → 仅纯重投影 O(n)(无结构重建/分配);
    // seek 回退或场景切换 → 结构整体重建。
    private sealed class TrailCache
    {
        public readonly List<List<Vec3>> Runs = new();          // 各连续实线段的模型顶点(jump 处分段)
        public List<Vec3>? OpenRun;                             // 当前开放段
        public readonly List<(Vec3 A, Vec3 B)> Jumps = new();   // jump 段(模型坐标)
        public readonly List<PolylineGeometry> Geos = new();    // Runs 投影后的几何(缓存)
        public StreamGeometry? JumpGeo;                          // 所有 jump 段合一条几何(批量描边)
        public int NextIndex = 1;                               // 下一个待处理 keyframe
        public int SinceKept;                                   // 距上次纳入的顶点数(抽稀计数)
        public Vec3 LastModel;                                  // 最后纳入顶点(活动段起点,模型坐标)
        public bool Started;
        public bool GeoDirty = true;                            // 几何需重投影(新增顶点 / 相机变)
    }

    private readonly Dictionary<SpatialObject, TrailCache> _trailCache = new();

    // 上次缓存对应的投影参数 / 播放头 / 抽稀步长(投影变 → 重投影;回退或步长变 → 结构重建)。
    private double _pcYaw = double.NaN, _pcPitch, _pcScale, _pcCx, _pcCy, _pcNow;
    private int _pcStride = 1;

    // 每帧绘制段数预算:总段数超此值即按 stride 降采样,把帧时间钉在预算内(与对象数/密度解耦)。
    // 取值留足余量(实测 ~5 万段仍流畅),常规场景 stride=1 不抽稀;仅 100+ 对象的极端密度才触发。
    private const int BudgetSegments = 60000;

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
    // 相机 / 人头朝向的 cos/sin 每帧只随角度变一次,缓存避免每个顶点重复算三角函数
    // (累积轨迹顶点随播放时长增长,这是"越久越卡"的最大常数项)。Render 开头刷新。
    private double _cosYaw = 1, _sinYaw, _cosPitch = 1, _sinPitch;
    private double _cosHYaw = 1, _sinHYaw, _cosHPitch = 1, _sinHPitch, _cosHRoll = 1, _sinHRoll;

    private void UpdateRotationCache()
    {
        _cosYaw = Math.Cos(_yaw);
        _sinYaw = Math.Sin(_yaw);
        _cosPitch = Math.Cos(_pitch);
        _sinPitch = Math.Sin(_pitch);
        _cosHYaw = Math.Cos(_headYaw);
        _sinHYaw = Math.Sin(_headYaw);
        _cosHPitch = Math.Cos(_headPitch);
        _sinHPitch = Math.Sin(_headPitch);
        _cosHRoll = Math.Cos(_headRoll);
        _sinHRoll = Math.Sin(_headRoll);
    }

    private Vec3 RotateCamera(Vec3 p)
    {
        double x1 = (p.X * _cosYaw) - (p.Y * _sinYaw);
        double y1 = (p.X * _sinYaw) + (p.Y * _cosYaw);
        double y2 = (y1 * _cosPitch) - (p.Z * _sinPitch);
        double z2 = (y1 * _sinPitch) + (p.Z * _cosPitch);
        return new Vec3(x1, y2, z2);
    }

    private Vec3 RotateHead(Vec3 p)
    {
        // roll(绕 Y)→ pitch(绕 X)→ yaw(绕 Z)
        double x1 = (p.X * _cosHRoll) + (p.Z * _sinHRoll);
        double z1 = (-p.X * _sinHRoll) + (p.Z * _cosHRoll);
        double y1 = p.Y;
        double y2 = (y1 * _cosHPitch) - (z1 * _sinHPitch);
        double z2 = (y1 * _sinHPitch) + (z1 * _cosHPitch);
        double x3 = (x1 * _cosHYaw) - (y2 * _sinHYaw);
        double y3 = (x1 * _sinHYaw) + (y2 * _cosHYaw);
        return new Vec3(x3, y3, z2);
    }

    private double _cx;
    private double _cy;
    private double _scale;

    private Point Project(Vec3 v) => new(_cx + (_scale * v.X), _cy - (_scale * v.Z));

    public override void Render(DrawingContext context)
    {
        var b = Bounds;
        if (!ReferenceEquals(Scene, _lastScene))
        {
            _labels.Clear();
            _trailCache.Clear();
            _lastScene = Scene;
        }

        context.FillRectangle(s_background, new Rect(0, 0, b.Width, b.Height));

        _cx = b.Width / 2.0;
        _cy = b.Height / 2.0;
        _scale = Math.Min(b.Width, b.Height) / 2.0 * 0.78 * _zoom;
        UpdateRotationCache();

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
        _samples.Clear();
        if (scene is not null)
        {
            foreach (var obj in scene.Objects)
            {
                if (obj.IsBed)
                {
                    continue;
                }

                var s = obj.SampleAt(CurrentTime);
                _samples.Add((obj, s, RotateCamera(s.Position)));
            }
        }

        // 远批(深度 > 0,在人头后)先画。
        foreach (var item in _samples)
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
        foreach (var item in _samples)
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
        // 背面剔除:凸立方体只有朝相机的面(旋转后法线 Y<0,指向观察者)可见,且彼此不重叠 →
        // 无需画家排序,直接画可见面即可。相比画全 6 面 + 排序,几何量与分配都大幅减少。
        foreach (var f in s_faces)
        {
            if (RotateCamera(RotateHead(f.Normal)).Y >= 0)
            {
                continue;
            }

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

        context.DrawGeometry(new ImmutableSolidColorBrush(color), null, geo);
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
        double t0 = Math.Max(0.0, now - TrailSeconds);

        // 自适应抽稀:估算本帧总段数(累积≈已开始 block 数,拖尾≈窗口内 block 数),
        // 超预算则 stride>1 跳采样,把每帧绘制段数钉在预算内。
        long est = 0;
        foreach (var obj in scene.Objects)
        {
            if (obj.IsBed || obj.Keyframes.Count < 2)
            {
                continue;
            }

            est += persist
                ? obj.Keyframes.Count
                : Math.Max(0, obj.IndexAtOrBefore(now) - obj.IndexAtOrBefore(t0) + 1);
        }

        int stride = (int)Math.Max(1, (est + BudgetSegments - 1) / BudgetSegments);

        bool camChanged = false;
        if (persist)
        {
            // 投影参数变(相机旋转/缩放/尺寸)→ 仅重投影(结构保留);seek 回退或 stride 变 → 结构重建。
            camChanged = _yaw != _pcYaw || _pitch != _pcPitch || _scale != _pcScale ||
                _cx != _pcCx || _cy != _pcCy;
            if (now < _pcNow - 1e-6 || stride != _pcStride)
            {
                _trailCache.Clear();
            }

            _pcYaw = _yaw;
            _pcPitch = _pitch;
            _pcScale = _scale;
            _pcCx = _cx;
            _pcCy = _cy;
            _pcNow = now;
            _pcStride = stride;
        }

        foreach (var obj in scene.Objects)
        {
            if (obj.IsBed || obj.Keyframes.Count < 2)
            {
                continue;
            }

            Color baseCol = ObjectColor(obj.ColorIndex);
            if (persist)
            {
                DrawPersistTrail(context, obj, now, baseCol, camChanged, stride);
            }
            else
            {
                DrawFadeTrail(context, obj, now, baseCol, stride);
            }
        }
    }

    // 拖尾:只画播放头前 TrailSeconds 这段滑动窗口,越接近"现在"越亮越粗(彗尾)。
    // 顶点落在真实轨迹拐点(各 block 插值起点/终点这些固定绝对时刻)上 → 中间不抽动,
    // 只有窗口两端 t0/now 随播放头沿当前直线段平滑平移。
    private void DrawFadeTrail(DrawingContext context, SpatialObject obj, double now, Color col, int stride)
    {
        double t0 = Math.Max(0.0, now - TrailSeconds);
        double span = now - t0;
        if (span < 1e-4)
        {
            return;
        }

        // 收集窗口内真实轨迹的时间断点(标注是否为瞬移点),加上两端,排序。
        // JumpInto=true 表示到达该时刻意味着位置在此刻阶跃(进入 jump block)→ 这一段画虚线。
        // 只遍历窗口 [t0, now] 覆盖的 keyframe 子区间(二分定位起点),而非全部 → 与 block 总数无关。
        // stride>1 时跳采样(段数超预算时降采样)。
        var pts = _trailPts;
        pts.Clear();
        pts.Add((t0, false));
        var kfs = obj.Keyframes;
        for (int i = Math.Max(0, obj.IndexAtOrBefore(t0)); i < kfs.Count; i += stride)
        {
            var kf = kfs[i];
            double a = kf.TimeSeconds;
            if (a >= now)
            {
                break; // 已越过窗口右端,后面更晚,无需再看
            }

            if (a > t0)
            {
                pts.Add((a, kf.Jump));
            }

            if (stride == 1)
            {
                double b = a + kf.InterpSeconds;
                if (b > t0 && b < now && b > a)
                {
                    pts.Add((b, false)); // 插值终点(jump 时 b==a,跳过)
                }
            }
        }

        pts.Add((now, false));
        pts.Sort((x, y) => x.T.CompareTo(y.T));

        Point prev = Project(RotateCamera(obj.SampleAt(pts[0].T).Position));
        for (int i = 1; i < pts.Count; i++)
        {
            var smp = obj.SampleAt(pts[i].T);
            Point cur = Project(RotateCamera(smp.Position));
            double f = (pts[i].T - t0) / span; // 0 = 尾(旧)→ 1 = 头(现在)
            double alpha = 0.5 * f * (smp.Active ? 1.0 : 0.35);
            double w = 0.8 + (2.0 * f);
            IPen pen = pts[i].JumpInto
                ? DashPen(col, alpha, 1.2)
                : new ImmutablePen(new ImmutableSolidColorBrush(WithAlpha(col, alpha)), w);
            context.DrawLine(pen, prev, cur);
            prev = cur;
        }
    }

    // 瞬移段虚线 pen(jumpPosition):dash 单位是线宽倍数。
    private static ImmutablePen DashPen(Color col, double alpha, double width) =>
        new(new ImmutableSolidColorBrush(WithAlpha(col, alpha)), width,
            new ImmutableDashStyle(new double[] { 3, 3 }, 0));

    // 累积:连接固定的轨迹拐点(每个已开始 block 的目标位置 = keyframe 顶点)直到播放头,
    // 末端在插值中途时收到当前实时位置。顶点不随播放头变 → 真"足迹",不抽动。
    // 跨帧缓存:已 arrived 的顶点投影固定 → 缓存折线几何,单调播放时只增量追加,每帧 O(1)。
    private void DrawPersistTrail(DrawingContext context, SpatialObject obj, double now, Color col, bool camChanged,
        int stride)
    {
        var kfs = obj.Keyframes;
        if (!_trailCache.TryGetValue(obj, out var cache))
        {
            cache = new TrailCache();
            _trailCache[obj] = cache;
        }

        // 初始化起点(对象首块开始之后)。
        if (!cache.Started)
        {
            if (kfs[0].TimeSeconds > now)
            {
                return; // 对象尚未开始
            }

            cache.LastModel = kfs[0].Position;
            cache.Started = true;
        }

        // 增量推进:把新 arrived 的顶点并入模型结构(jump 打断实线段、记一条虚线段)。
        // stride>1 时只每隔 stride 个顶点纳入一次(降采样),跳过的顶点不画、不更新活动段起点。
        while (cache.NextIndex < kfs.Count)
        {
            var kf = kfs[cache.NextIndex];
            if (kf.TimeSeconds > now || kf.TimeSeconds + kf.InterpSeconds > now)
            {
                break; // 还没开始,或末端仍在插值中(留给活动段)
            }

            cache.SinceKept++;
            if (cache.SinceKept < stride)
            {
                cache.NextIndex++;
                continue; // 抽稀:跳过该顶点
            }

            cache.SinceKept = 0;
            if (kf.Jump)
            {
                cache.Jumps.Add((cache.LastModel, kf.Position));
                cache.OpenRun = null; // 打断连续实线
            }
            else if (cache.OpenRun is { } run)
            {
                run.Add(kf.Position);
            }
            else
            {
                var r = new List<Vec3> { cache.LastModel, kf.Position };
                cache.Runs.Add(r);
                cache.OpenRun = r;
            }

            cache.LastModel = kf.Position;
            cache.NextIndex++;
            cache.GeoDirty = true;
        }

        // 仅在新增顶点或相机变化时重投影(否则复用缓存几何)。
        if (cache.GeoDirty || camChanged)
        {
            cache.Geos.Clear();
            foreach (var run in cache.Runs)
            {
                var pts = new Point[run.Count];
                for (int i = 0; i < run.Count; i++)
                {
                    pts[i] = Project(RotateCamera(run[i]));
                }

                cache.Geos.Add(new PolylineGeometry(pts, false));
            }

            // 所有 jump 段合进一条几何(每段一个 figure)→ 一次描边,而非每段 DrawLine。
            if (cache.Jumps.Count > 0)
            {
                var jg = new StreamGeometry();
                using var gc = jg.Open();
                foreach (var (a, b) in cache.Jumps)
                {
                    gc.BeginFigure(Project(RotateCamera(a)), false);
                    gc.LineTo(Project(RotateCamera(b)));
                    gc.EndFigure(false);
                }

                cache.JumpGeo = jg;
            }
            else
            {
                cache.JumpGeo = null;
            }

            cache.GeoDirty = false;
        }

        // 绘制:定型实线段(各一次描边)+ 定型 jump 虚线(一次)+ 末端活动段(实时插值,每帧一条)。
        var solid = new ImmutablePen(new ImmutableSolidColorBrush(WithAlpha(col, 0.42)), 1.4);
        foreach (var geo in cache.Geos)
        {
            context.DrawGeometry(null, solid, geo);
        }

        if (cache.JumpGeo is { } jumpGeo)
        {
            context.DrawGeometry(null, DashPen(col, 0.42, 1.2), jumpGeo);
        }

        if (cache.Started)
        {
            Point last = Project(RotateCamera(cache.LastModel));
            Point live = Project(RotateCamera(obj.SampleAt(now).Position));
            if (live != last)
            {
                context.DrawLine(solid, last, live);
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
        context.DrawEllipse(new ImmutableSolidColorBrush(WithAlpha(col, 0.18 * op)), null, p, glowR, glowR);
        // 核心点(透明度随增益)
        double gainOp = op * Math.Clamp(s.Gain, 0.15, 1.0);
        context.DrawEllipse(new ImmutableSolidColorBrush(WithAlpha(col, gainOp)),
            new ImmutablePen(new ImmutableSolidColorBrush(WithAlpha(Colors.White, 0.6 * op)), 1.0), p, coreR, coreR);

        if (ShowLabels && s.Active && !string.IsNullOrEmpty(obj.Name))
        {
            context.DrawText(Label(obj.Name), new Point(p.X + glowR + 3, p.Y - 7));
        }
    }

    // 缓存的对象名 FormattedText(文本/字号/颜色恒定)。仅活跃对象用,位置每帧另给。
    private FormattedText Label(string name)
    {
        if (!_labels.TryGetValue(name, out var ft))
        {
            ft = new FormattedText(name, CultureInfo.CurrentCulture, FlowDirection.LeftToRight,
                Typeface.Default, 11, new ImmutableSolidColorBrush(WithAlpha(Colors.White, 0.85)));
            _labels[name] = ft;
        }

        return ft;
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

            var brush = new ImmutableSolidColorBrush(WithAlpha(ObjectColor(obj.ColorIndex), 0.55));
            foreach (var bp in obj.BedPoints)
            {
                Point p = Project(RotateCamera(bp.Position));
                context.FillRectangle(brush, new Rect(p.X - 3, p.Y - 3, 6, 6));
            }
        }
    }

    // ── 配色 / 工具 ──
    private static readonly IBrush s_background = new ImmutableSolidColorBrush(Color.FromRgb(0x16, 0x18, 0x1C));
    private static readonly IPen s_cubePen =
        new ImmutablePen(new ImmutableSolidColorBrush(Color.FromArgb(0x55, 0xAE, 0xAE, 0xB2)), 1.0);

    private static readonly Color s_skin = Color.FromRgb(0xBB, 0x8B, 0x61);
    private static readonly Color s_skinDark = Color.FromRgb(0x9C, 0x70, 0x4C);
    private static readonly Color s_hair = Color.FromRgb(0x45, 0x30, 0x1E);
    private static readonly Color s_eyeWhite = Color.FromRgb(0xE6, 0xE6, 0xE6);
    private static readonly Color s_iris = Color.FromRgb(0x4A, 0x5B, 0x8C);
    private static readonly Color s_mouth = Color.FromRgb(0x6B, 0x4A, 0x38);

    // 人头 6 面(几何 + 法线 + 底色恒定 → 静态一次构建,不每帧分配)。UV 序角点见 Face。
    private static readonly Face[] s_faces = BuildFaces();

    private static Face[] BuildFaces()
    {
        const double h = HeadHalf;
        return new[]
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
    }

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
