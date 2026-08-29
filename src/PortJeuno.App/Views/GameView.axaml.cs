using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using PortJeuno.App.Controls;
using PortJeuno.App.ViewModels;

namespace PortJeuno.App.Views;

public partial class GameView : UserControl
{
    private GameViewModel? _bound;

    public GameView()
    {
        InitializeComponent();

        // Arrow keys steer. Handled at the tunnelling stage so the view sees
        // them before a focused control does, but ignored while a TextBox has
        // focus - otherwise arrowing through a half-typed message would walk
        // the character across the zone.
        AddHandler(KeyDownEvent, OnKeyDown, RoutingStrategies.Tunnel);

        DataContextChanged += OnDataContextChanged;
    }

    protected override void OnLoaded(RoutedEventArgs e)
    {
        base.OnLoaded(e);
        Focus();
        Redraw();
    }

    private void OnDataContextChanged(object? sender, System.EventArgs e)
    {
        if (_bound is not null)
        {
            _bound.RadarChanged -= Redraw;
        }

        _bound = DataContext as GameViewModel;

        if (_bound is not null)
        {
            _bound.RadarChanged += Redraw;
        }

        // The radar is drawn rather than bound, so it is pushed to rather than
        // pulling - see RadarControl for why bindings were abandoned here.
        RadarControl radar = this.FindControl<RadarControl>("Radar")!;
        radar.Clicked -= OnRadarClicked;
        radar.Clicked += OnRadarClicked;
    }

    private void OnRadarClicked(float x, float depth) => _bound?.WalkTo(x, depth);

    private void Redraw()
    {
        if (_bound is null)
        {
            return;
        }

        this.FindControl<RadarControl>("Radar")?.Update(
            _bound.CentreX, _bound.CentreDepth, _bound.MapPolygons, _bound.Blips, _bound.RoutePoints);
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
