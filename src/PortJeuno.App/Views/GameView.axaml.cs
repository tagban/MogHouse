using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using PortJeuno.App.ViewModels;

namespace PortJeuno.App.Views;

public partial class GameView : UserControl
{
    public GameView()
    {
        InitializeComponent();

        // Arrow keys steer. Handled at the tunnelling stage so the view sees
        // them before a focused control does, but ignored while a TextBox has
        // focus - otherwise arrowing through a half-typed message would walk
        // the character across the zone.
        AddHandler(KeyDownEvent, OnKeyDown, RoutingStrategies.Tunnel);
    }

    protected override void OnLoaded(RoutedEventArgs e)
    {
        base.OnLoaded(e);
        Focus();
    }

    /// <summary>Enter sends. Bound on the chat box itself so it never competes with movement keys.</summary>
    private void OnChatKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter || DataContext is not GameViewModel vm)
        {
            return;
        }

        if (vm.SendCommand.CanExecute(null))
        {
            vm.SendCommand.Execute(null);
        }

        e.Handled = true;
    }

    private void OnKeyDown(object? sender, KeyEventArgs e)
    {
        if (DataContext is not GameViewModel vm || e.Source is TextBox)
        {
            return;
        }

        string? direction = e.Key switch
        {
            Key.Up or Key.W => "north",
            Key.Down or Key.S => "south",
            Key.Left or Key.A => "west",
            Key.Right or Key.D => "east",
            _ => null,
        };

        if (direction is null)
        {
            return;
        }

        vm.Move(direction);
        e.Handled = true;
    }
}
