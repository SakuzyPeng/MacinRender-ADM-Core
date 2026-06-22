using System;
using System.Runtime.InteropServices;

namespace MacinRender.Gui.Interop;

// Blittable mirrors of the polled monitor structs in include/adm/c_api.h (v1.15+).
// Each carries a struct_size the caller sets to sizeof before the call; the library writes
// only the fields that fit, so these stay forward-compatible as fields are appended.

[StructLayout(LayoutKind.Sequential)]
public struct AdmMonitorStatus
{
    public uint StructSize;
    public int State; // AdmMonitorState
    public ulong PlayheadFrames;
    public ulong Underruns;
    public ulong BufferedFrames;
    public float RingFill; // 0..1
    public int Ended;      // bool
    public int Failed;     // bool
    public ulong OverrideRevision;
}

// Caller provides Peak/Rms float buffers of Capacity; the library writes
// min(Capacity, channels) values and sets OutCount. Peak/Rms may be IntPtr.Zero to skip.
[StructLayout(LayoutKind.Sequential)]
public struct AdmMonitorLevels
{
    public uint StructSize;
    public uint Capacity;
    public uint OutCount;
    public IntPtr Peak;
    public IntPtr Rms;

    // v1.18: program loudness (LUFS, ITU-R BS.1770) of the monitored output. -inf below the
    // gate / silent. Only written when StructSize covers them (set StructSize = sizeof before).
    public float MomentaryLufs;
    public float ShorttermLufs;
    public float IntegratedLufs;
}

// A single object's live override (object_id is a UTF-8 char* the caller owns for the call).
[StructLayout(LayoutKind.Sequential)]
public struct AdmMonitorOverride
{
    public uint StructSize;
    public IntPtr ObjectId; // const char* (UTF-8)
    public float GainDb;
    public float DiffuseScale;
    public float ExtentScale;
    public float DivergenceScale;
    public float ExtentWidthScale;
    public float ExtentHeightScale;
    public float ExtentDepthScale;

    // v1.20: optional DirectSpeakers channel filter (UTF-8 char*, caller-owned for the call).
    // Non-NULL restricts the override to one bed channel; IntPtr.Zero = whole object.
    public IntPtr SpeakerLabel;
}
