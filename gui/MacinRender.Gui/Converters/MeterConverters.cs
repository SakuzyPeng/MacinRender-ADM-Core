using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia.Data.Converters;
using MacinRender.Gui.ViewModels;

namespace MacinRender.Gui.Converters;

/// <summary>
/// 竖条填充高度(像素)= Norm(峰值) × track 实际高度。两个输入:[0]=线性峰值(float),[1]=track 高度(double)。
/// 让竖条按 -60..0 dBFS 正确缩放(-50 dB ≈ 17%,不会顶满),同时随窗口拉高而等比变大;
/// 高度变化由 XAML 的 DoubleTransition 平滑过渡(弹道仍由轮询 SmoothPeak 决定)。
/// </summary>
public sealed class MeterFillHeightConverter : IMultiValueConverter
{
    public object Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
    {
        if (values.Count >= 2 && values[0] is float peak && values[1] is double trackHeight && trackHeight > 0.0)
        {
            return MeterScale.Norm(peak) * trackHeight;
        }

        return 0.0;
    }
}
