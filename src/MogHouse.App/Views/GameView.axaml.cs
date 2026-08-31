using Avalonia.Controls;
using Avalonia.Input;

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

    /// <summary>Enter sends. Bound on the chat box itself.</summary>
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
