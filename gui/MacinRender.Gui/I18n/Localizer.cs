using System.Collections.Generic;
using System.ComponentModel;

namespace MacinRender.Gui.I18n;

public enum Lang
{
    Zh,
    En
}

/// <summary>
/// 运行时字典国际化:纯托管,零依赖,AOT/InvariantGlobalization 友好。
/// 静态 XAML 文案经 {DynamicResource Key}——由 App 把当前语言字典灌入 Application.Resources,
/// 切语言时 App 重新灌入即刷新(DynamicResource 是 Avalonia 核心机制,无反射)。
/// VM 动态文案直接用 this[key] / Format(...),切语言时各自 OnPropertyChanged / 日志 RefreshLanguage。
/// 专名(SAF VBAP/PCM/7.1.4/Binaural 等下拉项)不入字典,中英一致由 OutputModel 直接给。
/// </summary>
public sealed class Localizer : INotifyPropertyChanged
{
    public static Localizer Instance { get; } = new();

    private Localizer()
    {
    }

    private Lang _current = Lang.Zh;

    public Lang Current => _current;

    public string this[string key] =>
        (_current == Lang.En ? En : Zh).TryGetValue(key, out var value) ? value : key;

    /// <summary>全部 key(供 App 灌入 {DynamicResource} 资源)。</summary>
    public IEnumerable<string> Keys => Zh.Keys;

    /// <summary>带参数格式化(用于状态栏等含动态数据的文案)。</summary>
    public string Format(string key, params object?[] args) => string.Format(this[key], args);

    public void SetLanguage(Lang lang)
    {
        if (_current == lang)
        {
            return;
        }

        _current = lang;
        // App(重灌资源) 与 VM(刷新文案) 订阅此事件,忽略具体名,任意 raise 即触发。
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Current)));
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private static readonly Dictionary<string, string> Zh = new()
    {
        // ── 静态 UI ──
        ["AppSubtitle"] = "ADM 空间音频渲染",
        ["NavRender"] = "渲染",
        ["NavSemantic"] = "语义编辑",
        ["PageDesc"] = "导入 ADM BWF,选择渲染后端与输出格式,批量离线渲染",
        // ── 语义编辑 ──
        ["SemanticPageDesc"] = "载入单个 ADM,逐对象编辑听感语义并渲染",
        ["SemLoadFile"] = "载入文件",
        ["SemFileHeader"] = "文件",
        ["SemNoFile"] = "未载入文件",
        ["SemLoading"] = "载入中:{0}",
        ["SemLoaded"] = "已载入 — {0} 个对象",
        ["SemLoadFailed"] = "载入失败",
        ["SemObjects"] = "对象",
        ["SemPrefix"] = "公共前缀(已隐藏):{0}",
        ["SemColCurrent"] = "当前值",
        ["SemColOverride"] = "覆盖(相对)",
        ["SemColEffective"] = "生效值",
        ["SemSelectHint"] = "在左侧选择一个对象开始编辑",
        ["SemNoOverride"] = "尚无覆盖",
        ["SemOverrideN"] = "{0} 个对象已设覆盖",
        // ── 实时监听 ──
        ["SemMonitor"] = "监听",
        ["SemMonStart"] = "开始监听",
        ["SemMonStop"] = "停止监听",
        ["SemMonStarting"] = "启动监听中…",
        ["SemMonStartFailed"] = "监听启动失败:{0}",
        ["SemMonSwitchFailed"] = "后端切换失败:{0}",
        ["SemMonEnded"] = "已到素材结尾",
        ["SemMonUnderruns"] = "欠载 {0} 次",
        ["SemMonFailed"] = "监听渲染出错",
        ["SemMonToStart"] = "回到开头",
        ["SemMonBackend"] = "监听后端",
        ["SemSofaLabel"] = "自定义 HRIR(SOFA)",
        ["SemSofaNone"] = "未选择(默认 KEMAR)",
        ["SemSofaPick"] = "选择…",
        ["SemSofaClear"] = "清除",
        ["SemExport"] = "导出生效 ADM",
        ["SemExporting"] = "正在导出生效 ADM…",
        ["SemExported"] = "已导出 {0}",
        ["SemExportFailed"] = "导出失败:{0}",
        ["SemResetHint"] = "双击「对象」清空全部 · 双击对象名清空该对象 · 双击维度名清空该项",
        ["SwitchTheme"] = "切换主题",
        ["OutputSettings"] = "输出设置",
        ["Backend"] = "渲染后端",
        ["Layout"] = "输出布局",
        ["Codec"] = "编码器",
        ["Container"] = "容器",
        ["FileQueue"] = "文件队列",
        ["EventLog"] = "事件 / 日志",
        ["Copied"] = "已复制 ✓",
        ["TipAddFile"] = "添加文件",
        ["TipAddFolder"] = "添加文件夹",
        ["TipClearQueue"] = "清空队列",
        ["TipClearLog"] = "清空日志",
        ["CopyAll"] = "复制全部",
        // ── 按钮 / 子选项 ──
        ["StartRender"] = "开始渲染",
        ["Cancel"] = "取消",
        ["BitratePerCh"] = "码率 kbps/声道",
        ["BitrateTotal"] = "码率 kbps 总",
        ["FixedNoteFlac"] = "固定 24-bit",
        // ── 状态栏(含 {0}/{1} 占位) ──
        ["StatusReadyEmpty"] = "就绪 — 队列为空",
        ["StatusReadyN"] = "就绪 — 队列中 {0} 个文件",
        ["StatusRendering"] = "渲染中 ({0}/{1}) — {2}",
        ["StatusDone"] = "完成 — {0}/{1} 个文件",
        ["StatusCancelled"] = "已取消",
        ["StatusQueueEmpty"] = "队列为空",
        ["StatusQueueCleared"] = "队列已清空",
        // ── 日志 ──
        ["LogReady"] = "就绪",
        ["LogReadyHint"] = "点「添加文件」导入 ADM BWF",
        ["LogAddFile"] = "添加文件",
        ["LogSkipNonAdm"] = "跳过非 ADM 文件",
        ["LogStart"] = "开始渲染",
        ["LogDone"] = "已完成",
        ["LogFailed"] = "渲染失败",
        ["LogAllDone"] = "全部完成",
        ["LogCancelled"] = "已取消",
        ["LogCancelDetail"] = "用户中断渲染",
        ["LogNoFile"] = "无文件",
        ["LogNoFileHint"] = "先添加 ADM 文件再渲染",
        ["LogAllDoneDetail"] = "{0}/{1} 成功",
        // ── 阶段短词 ──
        ["StageValidate"] = "校验",
        ["StageImport"] = "导入",
        ["StagePlan"] = "准备",
        ["StageRender"] = "渲染",
        ["StageEncode"] = "编码",
        ["StagePackage"] = "封装",
        // ── 文件选择器 ──
        ["PickTitle"] = "选择 ADM BWF 文件",
        ["PickFolderTitle"] = "选择含 ADM BWF 的文件夹",
        ["PickFilter"] = "ADM 音频",
        ["NFiles"] = "{0} 个文件",
        ["Raw"] = "{0}", // 数据直通(文件名/路径等,不翻)
    };

    private static readonly Dictionary<string, string> En = new()
    {
        ["AppSubtitle"] = "ADM Spatial Audio Rendering",
        ["NavRender"] = "Render",
        ["NavSemantic"] = "Semantics",
        ["PageDesc"] = "Import ADM BWF, pick a renderer and output format, batch offline render",
        ["SemanticPageDesc"] = "Load one ADM file, edit per-object perceptual semantics, render",
        ["SemLoadFile"] = "Load file",
        ["SemFileHeader"] = "File",
        ["SemNoFile"] = "No file loaded",
        ["SemLoading"] = "Loading: {0}",
        ["SemLoaded"] = "Loaded — {0} object(s)",
        ["SemLoadFailed"] = "Load failed",
        ["SemObjects"] = "Objects",
        ["SemPrefix"] = "Common prefix (hidden): {0}",
        ["SemColCurrent"] = "Current",
        ["SemColOverride"] = "Override (relative)",
        ["SemColEffective"] = "Effective",
        ["SemSelectHint"] = "Select an object on the left to start editing",
        ["SemNoOverride"] = "No overrides yet",
        ["SemOverrideN"] = "{0} object(s) overridden",
        // ── Realtime monitoring ──
        ["SemMonitor"] = "Monitor",
        ["SemMonStart"] = "Start monitoring",
        ["SemMonStop"] = "Stop monitoring",
        ["SemMonStarting"] = "Starting monitor…",
        ["SemMonStartFailed"] = "Monitor failed to start: {0}",
        ["SemMonSwitchFailed"] = "Backend switch failed: {0}",
        ["SemMonEnded"] = "Reached end of material",
        ["SemMonUnderruns"] = "{0} underrun(s)",
        ["SemMonFailed"] = "Monitor render error",
        ["SemMonToStart"] = "To start",
        ["SemSofaLabel"] = "Custom HRIR (SOFA)",
        ["SemSofaNone"] = "None (default KEMAR)",
        ["SemSofaPick"] = "Choose…",
        ["SemSofaClear"] = "Clear",
        ["SemExport"] = "Export effective ADM",
        ["SemExporting"] = "Exporting effective ADM…",
        ["SemExported"] = "Exported {0}",
        ["SemExportFailed"] = "Export failed: {0}",
        ["SemResetHint"] = "Double-click \"Objects\" to reset all · object name to reset it · a dimension name to reset that field",
        ["SemMonBackend"] = "Monitor backend",
        ["SwitchTheme"] = "Theme",
        ["OutputSettings"] = "Output Settings",
        ["Backend"] = "Renderer",
        ["Layout"] = "Output Layout",
        ["Codec"] = "Codec",
        ["Container"] = "Container",
        ["FileQueue"] = "File Queue",
        ["EventLog"] = "Events / Log",
        ["Copied"] = "Copied ✓",
        ["TipAddFile"] = "Add files",
        ["TipAddFolder"] = "Add folder",
        ["TipClearQueue"] = "Clear queue",
        ["TipClearLog"] = "Clear log",
        ["CopyAll"] = "Copy all",
        ["StartRender"] = "Start Render",
        ["Cancel"] = "Cancel",
        ["BitratePerCh"] = "Bitrate kbps/ch",
        ["BitrateTotal"] = "Bitrate kbps total",
        ["FixedNoteFlac"] = "Fixed 24-bit",
        ["StatusReadyEmpty"] = "Ready — queue empty",
        ["StatusReadyN"] = "Ready — {0} file(s) queued",
        ["StatusRendering"] = "Rendering ({0}/{1}) — {2}",
        ["StatusDone"] = "Done — {0}/{1} file(s)",
        ["StatusCancelled"] = "Cancelled",
        ["StatusQueueEmpty"] = "Queue empty",
        ["StatusQueueCleared"] = "Queue cleared",
        ["LogReady"] = "Ready",
        ["LogReadyHint"] = "Click 'Add files' to import ADM BWF",
        ["LogAddFile"] = "Added files",
        ["LogSkipNonAdm"] = "Skipped non-ADM files",
        ["LogStart"] = "Render started",
        ["LogDone"] = "Done",
        ["LogFailed"] = "Render failed",
        ["LogAllDone"] = "All done",
        ["LogCancelled"] = "Cancelled",
        ["LogCancelDetail"] = "Cancelled by user",
        ["LogNoFile"] = "No files",
        ["LogNoFileHint"] = "Add ADM files before rendering",
        ["LogAllDoneDetail"] = "{0}/{1} succeeded",
        ["StageValidate"] = "Validate",
        ["StageImport"] = "Import",
        ["StagePlan"] = "Prepare",
        ["StageRender"] = "Render",
        ["StageEncode"] = "Encode",
        ["StagePackage"] = "Package",
        ["PickTitle"] = "Select ADM BWF files",
        ["PickFolderTitle"] = "Select folder with ADM BWF files",
        ["PickFilter"] = "ADM audio",
        ["NFiles"] = "{0} files",
        ["Raw"] = "{0}",
    };
}
