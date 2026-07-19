using System;
using System.ComponentModel;
using Avalonia.Media;
using CommunityToolkit.Mvvm.ComponentModel;
using MacinRender.Gui.I18n;

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

    /// <summary>渲染中的阶段 i18n key(StageValidate/StageImport/…),由进度回调更新。</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(StageText))]
    private string _stageKey = "";

    /// <summary>阶段短词:据 StageKey 本地化;语言切换时由 RefreshLanguage 刷新。</summary>
    public string StageText => string.IsNullOrEmpty(StageKey) ? "" : Localizer.Instance[StageKey];

    /// <summary>语言切换时刷新阶段和状态文本(枚举 / StageKey 不变,只重算本地化结果)。</summary>
    public void RefreshLanguage()
    {
        OnPropertyChanged(nameof(StageText));
        OnPropertyChanged(nameof(StatusText));
    }

    public bool IsPending => Status == FileStatus.Pending;
    public bool IsRenderingState => Status == FileStatus.Rendering;
    public bool IsDone => Status == FileStatus.Done;
    public bool IsFailed => Status == FileStatus.Failed;

    public string StatusText => Status switch
    {
        FileStatus.Pending => Localizer.Instance["QueueStatusPending"],
        FileStatus.Rendering => Localizer.Instance["QueueStatusRendering"],
        FileStatus.Done => Localizer.Instance["QueueStatusDone"],
        FileStatus.Failed => Localizer.Instance["QueueStatusFailed"],
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
/// 一条「事件 / 日志」:标题/详情用 i18n key + 参数惰性求值,语言切换时由 RefreshLanguage 刷新。
/// detailKey 为空串 = 无详情;数据型详情(文件名/路径)用 "Raw" key("{0}")直通不翻。
/// </summary>
public sealed class LogLine : INotifyPropertyChanged
{
    private readonly string _titleKey;
    private readonly string _detailKey;
    private readonly object?[] _detailArgs;

    public LogLine(LogKind kind, string titleKey, string detailKey, object?[] detailArgs, DateTime timestamp)
    {
        Kind = kind;
        _titleKey = titleKey;
        _detailKey = detailKey;
        _detailArgs = detailArgs;
        Time = timestamp.ToString("HH:mm:ss");
    }

    public LogKind Kind { get; }
    public string Time { get; }
    public string Title => Localizer.Instance[_titleKey];
    public string Detail => string.IsNullOrEmpty(_detailKey) ? "" : Localizer.Instance.Format(_detailKey, _detailArgs);
    public bool HasDetail => !string.IsNullOrEmpty(_detailKey);

    public IBrush KindBrush => new SolidColorBrush(Kind switch
    {
        LogKind.Info => Color.Parse("#8E8E93"),
        LogKind.Success => Color.Parse("#30D158"),
        LogKind.Warn => Color.Parse("#FF9F0A"),
        LogKind.Error => Color.Parse("#FF453A"),
        _ => Colors.Gray
    });

    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>语言切换时由 ViewModel 调用,刷新 Title/Detail 绑定(无须订阅 Localizer,避免泄漏)。</summary>
    public void RefreshLanguage()
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Title)));
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Detail)));
    }
}
