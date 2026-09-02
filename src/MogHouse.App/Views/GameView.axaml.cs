using MogHouse.Core.Ffxi;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace MogHouse.App.Views;

/// <summary>
/// The page beside the world window. It used to be the client - a drawn radar,
/// click-to-walk, arrow keys - and all of that moved into the renderer, so what
/// is left here is chat and a report on what the world is doing.
/// </summary>
public partial class GameView : UserControl
{
    public GameView()
    {
        InitializeComponent();
    }

    /// <summary>
    /// A chat line with a link in it opens that link when clicked.
    ///
    /// The server hands out links the moment you zone in, and one you cannot
    /// click is one you have to retype. Only http and https are recognised -
    /// chat is other people's text, and this client should not offer to launch
    /// whatever scheme somebody types into it.
    /// </summary>
    private void OnChatLineTapped(object? sender, TappedEventArgs e)
    {
        if (sender is Control { DataContext: MogHouse.Core.Ffxi.FfxiChatLine line })
        {
            Links.Open(Links.FirstIn(line.Text));
        }
    }

    /// <summary>Enter sends. Bound on the chat box itself.</summary>
    private void OnReportBug(object? sender, RoutedEventArgs e) => Links.Open(Links.Issues);

    private void OnOpenDiscord(object? sender, RoutedEventArgs e) => Links.Open(Links.Discord);

    private void OnChatKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter || DataContext is not ViewModels.GameViewModel vm)
        {
            return;
        }

        if (vm.SendCommand.CanExecute(null))
        {
            vm.SendCommand.Execute(null);
        }

        e.Handled = true;
    }
}
