using System;
using System.Globalization;
using Avalonia.Data.Converters;

namespace MogHouse.App;

/// <summary>
/// The first link in a chat line, for the tooltip that says where clicking it
/// goes. Null when there is none, which is what stops a tooltip appearing on
/// every line of ordinary conversation.
/// </summary>
public sealed class FirstLinkConverter : IValueConverter
{
    public static readonly FirstLinkConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        Links.FirstIn(value as string);

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
