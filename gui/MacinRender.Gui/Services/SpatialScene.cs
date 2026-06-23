using System;
using System.Collections.Generic;

namespace MacinRender.Gui.Services;

// 空间视图的几何模型,从 inspect JSON(InspectDoc)构建。纯数据 + 时间采样,不含绘制。
//
// 模型空间约定(ADM BS.2076,右手系):X=右(+)、Y=前(+)、Z=上(+);归一化立方体 [-1,1]^3。
// 极坐标(方位角 az:0=正前、+ 向左;仰角 el:0=水平、+ 向上;距离 dist:0..1)转笛卡尔:
//   x = -sin(az)·cos(el)·d   y = cos(az)·cos(el)·d   z = sin(el)·d

public readonly record struct Vec3(double X, double Y, double Z)
{
    public static Vec3 Lerp(Vec3 a, Vec3 b, double t) =>
        new(a.X + ((b.X - a.X) * t), a.Y + ((b.Y - a.Y) * t), a.Z + ((b.Z - a.Z) * t));
}

// 某一时刻一个对象的解算状态(供绘制)。
public readonly record struct SpatialSample(bool Active, Vec3 Position, double Width, double Height, double Depth,
    double Gain);

// 声床的静态点(每声道一个,环绕听者)。
public readonly record struct BedPoint(Vec3 Position, string Label);

// 对象的一帧关键帧(= 一个 ADM block);Position 是该 block 的目标位置。
public sealed class SpatialKeyframe
{
    public double TimeSeconds;   // block 起始
    public double EndSeconds;    // block 结束(末块取片尾)
    public Vec3 Position;
    public double Width;
    public double Height;
    public double Depth;
    public double Gain = 1.0;    // block 增益(线性)
    public double InterpSeconds; // 进入本 block 的插值时长(jump=0 则瞬跳)
    public bool Jump;            // jumpPosition:进入本 block 时瞬移(轨迹画虚线,非真实经过路径)
}

public sealed class SpatialObject
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public bool IsBed { get; init; }
    public int ColorIndex { get; init; }
    public double ObjectGain { get; init; } = 1.0;
    public bool Mute { get; init; }

    // 对象:运动关键帧序列(按时间升序)。声床:静态多点。
    public IReadOnlyList<SpatialKeyframe> Keyframes { get; init; } = Array.Empty<SpatialKeyframe>();
    public IReadOnlyList<BedPoint> BedPoints { get; init; } = Array.Empty<BedPoint>();

    // 在时刻 t 解算位置 / extent / 增益 / 是否活跃(线性插值近似,可视化用)。
    public SpatialSample SampleAt(double t)
    {
        if (Keyframes.Count == 0)
        {
            return new SpatialSample(false, default, 0, 0, 0, 0);
        }

        // 定位当前 block:最后一个 TimeSeconds <= t 的关键帧。
        int i = 0;
        for (int k = 0; k < Keyframes.Count; k++)
        {
            if (Keyframes[k].TimeSeconds <= t)
            {
                i = k;
            }
            else
            {
                break;
            }
        }

        var cur = Keyframes[i];
        Vec3 pos = cur.Position;
        // 从上一 block 目标位置插值进入当前 block(jump 时 InterpSeconds=0,即瞬跳)。
        if (i > 0 && cur.InterpSeconds > 0)
        {
            double f = (t - cur.TimeSeconds) / cur.InterpSeconds;
            if (f < 1.0)
            {
                pos = Vec3.Lerp(Keyframes[i - 1].Position, cur.Position, Math.Clamp(f, 0.0, 1.0));
            }
        }

        double gain = ObjectGain * cur.Gain;
        bool active = !Mute && gain > 0 && t >= cur.TimeSeconds && t < cur.EndSeconds;
        return new SpatialSample(active, pos, cur.Width, cur.Height, cur.Depth, gain);
    }
}

public sealed class SpatialScene
{
    public double DurationSeconds { get; init; }
    public IReadOnlyList<SpatialObject> Objects { get; init; } = Array.Empty<SpatialObject>();
    public bool IsEmpty => Objects.Count == 0;

    public static Vec3 FromPolar(double azDeg, double elDeg, double dist)
    {
        double az = azDeg * Math.PI / 180.0;
        double el = elDeg * Math.PI / 180.0;
        double ce = Math.Cos(el);
        return new Vec3(-Math.Sin(az) * ce * dist, Math.Cos(az) * ce * dist, Math.Sin(el) * dist);
    }

    internal static Vec3 FromPosition(InspectPosition p) =>
        p.Cartesian ? new Vec3(p.X, p.Y, p.Z) : FromPolar(p.Azimuth, p.Elevation, p.Distance);

    internal static SpatialScene Build(InspectDoc? doc)
    {
        if (doc is null)
        {
            return new SpatialScene();
        }

        double sr = doc.File is { SampleRate: > 0 } ? doc.File.SampleRate : 48000.0;
        double duration = doc.File?.DurationSeconds ?? 0.0;

        var objects = new List<SpatialObject>();
        int color = 0;
        foreach (var obj in doc.Objects)
        {
            bool isBed = !HasObjectBlocks(obj);
            if (isBed)
            {
                var points = new List<BedPoint>();
                foreach (var track in obj.Tracks)
                {
                    if (track.DsBlocks.Count == 0)
                    {
                        continue;
                    }

                    var ds = track.DsBlocks[0];
                    if (ds.HasPosition)
                    {
                        var label = ds.SpeakerLabels.Count > 0 ? ds.SpeakerLabels[0] : "";
                        points.Add(new BedPoint(FromPolar(ds.Azimuth, ds.Elevation, ds.Distance), label));
                    }
                }

                if (points.Count > 0)
                {
                    objects.Add(new SpatialObject
                    {
                        Id = obj.Id,
                        Name = obj.Name,
                        IsBed = true,
                        ColorIndex = color++,
                        ObjectGain = obj.Gain,
                        Mute = obj.Mute,
                        BedPoints = points,
                    });
                }

                continue;
            }

            // Objects:每个带 object_blocks 的 track = 一条运动轨迹(多数对象单 track)。
            int trackIdx = 0;
            int trackCount = CountObjectTracks(obj);
            foreach (var track in obj.Tracks)
            {
                if (track.ObjectBlocks.Count == 0)
                {
                    continue;
                }

                var kfs = BuildKeyframes(track.ObjectBlocks, sr, duration);
                string name = trackCount > 1 ? $"{obj.Name} ·{trackIdx + 1}" : obj.Name;
                objects.Add(new SpatialObject
                {
                    Id = obj.Id,
                    Name = name,
                    IsBed = false,
                    ColorIndex = color++,
                    ObjectGain = obj.Gain,
                    Mute = obj.Mute,
                    Keyframes = kfs,
                });
                trackIdx++;
            }
        }

        return new SpatialScene { DurationSeconds = duration, Objects = objects };
    }

    private static List<SpatialKeyframe> BuildKeyframes(List<InspectObjectBlock> blocks, double sr, double duration)
    {
        var kfs = new List<SpatialKeyframe>(blocks.Count);
        foreach (var blk in blocks)
        {
            double tStart = blk.StartSample / sr;
            double tEnd = blk.EndSample.HasValue ? blk.EndSample.Value / sr : Math.Max(duration, tStart);
            double interp = blk.InterpLengthSamples.HasValue
                ? blk.InterpLengthSamples.Value / sr
                : (blk.JumpPosition ? 0.0 : tEnd - tStart);
            kfs.Add(new SpatialKeyframe
            {
                TimeSeconds = tStart,
                EndSeconds = tEnd,
                Position = blk.Position is null ? default : FromPosition(blk.Position),
                Width = blk.Width,
                Height = blk.Height,
                Depth = blk.Depth,
                Gain = blk.Gain,
                InterpSeconds = interp,
                Jump = blk.JumpPosition,
            });
        }

        return kfs;
    }

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

    private static int CountObjectTracks(InspectObject obj)
    {
        int n = 0;
        foreach (var t in obj.Tracks)
        {
            if (t.ObjectBlocks.Count > 0)
            {
                n++;
            }
        }

        return n;
    }
}
