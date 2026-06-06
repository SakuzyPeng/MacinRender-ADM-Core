using System;
using Avalonia.Media;
using CommunityToolkit.Mvvm.ComponentModel;

namespace MacinRender.Gui.Models;

public enum FileStatus
{
    Pending,
    Rendering,
    Done,
    Failed
}

public enum LogKind
{
    Info,
    Success,
    Warn,
    Error
}

/// <summary>队列中的一个待渲染文件。</summary>
public partial class RenderFileItem : ObservableObject
{
    /// <summary>真实输入文件绝对路径(渲染时传给 AdmRenderService)。</summary>
    public string InputPath { get; init; } = "";

    [ObservableProperty]
    private string _name = "";

    [ObservableProperty]
    private double _progress;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusText))]
    [NotifyPropertyChangedFor(nameof(StatusBrush))]
    [NotifyPropertyChangedFor(nameof(IsPending))]
    [NotifyPropertyChangedFor(nameof(IsRenderingState))]
    [NotifyPropertyChangedFor(nameof(IsDone))]
    [NotifyPropertyChangedFor(nameof(IsFailed))]
    private FileStatus _status = FileStatus.Pending;

    /// <summary>渲染中的阶段短词(校验/导入/准备/渲染/编码/封装),由进度回调更新。</summary>
    [ObservableProperty]
    private string _stageText = "";

    public bool IsPending => Status == FileStatus.Pending;
    public bool IsRenderingState => Status == FileStatus.Rendering;
    public bool IsDone => Status == FileStatus.Done;
    public bool IsFailed => Status == FileStatus.Failed;

    public string StatusText => Status switch
    {
        FileStatus.Pending => "待渲染",
        FileStatus.Rendering => "渲染中",
        FileStatus.Done => "完成",
        FileStatus.Failed => "失败",
        _ => ""
    };

    public IBrush StatusBrush => new SolidColorBrush(Status switch
    {
        FileStatus.Pending => Color.Parse("#8E8E93"),
        FileStatus.Rendering => Color.Parse("#0A84FF"),
        FileStatus.Done => Color.Parse("#30D158"),
        FileStatus.Failed => Color.Parse("#FF453A"),
        _ => Colors.Gray
    });
}

/// <summary>
/// 一条「事件 / 日志」:标题 + 详情 + 时间(对齐老项目的结构化事件,非终端流水)。
/// </summary>
public sealed class LogLine
{
    public LogLine(LogKind kind, string title, string detail, DateTime timestamp)
    {
        Kind = kind;
        Title = title;
        Detail = detail;
        Time = timestamp.ToString("HH:mm:ss");
    }

    public LogKind Kind { get; }
    public string Title { get; }
    public string Detail { get; }
    public string Time { get; }
    public bool HasDetail => !string.IsNullOrEmpty(Detail);

    public IBrush KindBrush => new SolidColorBrush(Kind switch
    {
        LogKind.Info => Color.Parse("#8E8E93"),
        LogKind.Success => Color.Parse("#30D158"),
        LogKind.Warn => Color.Parse("#FF9F0A"),
        LogKind.Error => Color.Parse("#FF453A"),
        _ => Colors.Gray
    });
}
