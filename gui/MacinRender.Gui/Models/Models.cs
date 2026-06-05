using System;
using Avalonia.Media;
using CommunityToolkit.Mvvm.ComponentModel;

namespace MacinRender.Gui.Models;

public enum FileStatus
{
    Pending,
    Rendering,
    Done
}

public enum LogKind
{
    Info,
    Success,
    Warn,
    Error
}

/// <summary>队列中的一个待渲染文件(花瓶 mock,无真实数据)。</summary>
public partial class RenderFileItem : ObservableObject
{
    [ObservableProperty]
    private string _name = "";

    [ObservableProperty]
    private double _progress;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StatusText))]
    [NotifyPropertyChangedFor(nameof(StatusBrush))]
    private FileStatus _status = FileStatus.Pending;

    public string StatusText => Status switch
    {
        FileStatus.Pending => "待渲染",
        FileStatus.Rendering => "渲染中",
        FileStatus.Done => "完成",
        _ => ""
    };

    public IBrush StatusBrush => new SolidColorBrush(Status switch
    {
        FileStatus.Pending => Color.Parse("#8E8E93"),
        FileStatus.Rendering => Color.Parse("#0A84FF"),
        FileStatus.Done => Color.Parse("#30D158"),
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
