using System;
using Microsoft.Win32.SafeHandles;

namespace MacinRender.Gui.Interop;

// 每个 opaque adm_*_t 句柄一个 SafeHandle 子类:create 返回(marshaller 自动包装),
// Dispose/GC 时 ReleaseHandle 调对应 destroy,保证生命周期严格配对。
// SafeHandle 的引用计数让句柄在被 P/Invoke 调用期间不会被释放 —— 这正是
// adm_cancel 可从 UI 线程安全调用(而渲染在 worker 线程持有同一 token)的托管侧保障。

public sealed class AdmContextHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_context(handle);
        return true;
    }
}

public sealed class AdmRenderOptionsHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_render_options(handle);
        return true;
    }
}

public sealed class AdmCancelTokenHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_cancel_token(handle);
        return true;
    }
}

public sealed class AdmRenderResultHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_render_result(handle);
        return true;
    }
}

public sealed class AdmSceneInfoHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_scene_info(handle);
        return true;
    }
}

public sealed class AdmPreviewSessionHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_preview_session(handle);
        return true;
    }
}

public sealed class AdmMonitorHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
{
    protected override bool ReleaseHandle()
    {
        NativeMethods.adm_destroy_monitor(handle);
        return true;
    }
}
