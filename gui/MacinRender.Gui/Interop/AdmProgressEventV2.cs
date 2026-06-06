using System;
using System.Runtime.InteropServices;

namespace MacinRender.Gui.Interop;

/// <summary>
/// 严格对齐 c_api.h 的 adm_progress_event_v2_t(LayoutKind.Sequential,字段顺序与类型逐一对应)。
/// struct_size 由库侧填 sizeof,容许未来 minor 版本在尾部追加字段。
/// Message 是仅在回调期间有效的 const char*(UTF-8),需在蹦床内立即 PtrToStringUTF8。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct AdmProgressEventV2
{
    public uint StructSize;
    public AdmRenderStage Stage;
    public AdmProgressOperation Operation;
    public double OverallFraction;
    public double StageFraction;
    public ulong CurrentFrame;
    public ulong TotalFrames;
    public IntPtr Message;
}
