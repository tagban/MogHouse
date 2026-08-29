using System;
using System.Linq;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Avalonia.Media;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.App.ViewModels;

/// <summary>
/// One entity as a dot on the radar, already in canvas coordinates.
///
/// Colour is an IBrush rather than a string on purpose: a string works when
/// written literally in XAML but silently fails to convert through a binding,
/// which shows up as an empty canvas with no error anywhere.
/// </summary>
public sealed record EntityDot(double Left, double Top, string Label, IBrush Colour, double Size);

public partial class GameViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;

    /// <summary>Half the canvas edge, in pixels - the radar is square and centred on us.</summary>
    private const double Radius = 150;

    /// <summary>
    /// Game units shown from the centre to the edge. FFXI's own PC sync range
    /// is 45 units, so 50 covers everything the server will tell us about.
    /// </summary>
    private const double RangeInUnits = 50;

    private static readonly IBrush SelfBrush = new SolidColorBrush(Color.Parse("#4FC3F7"));
    private static readonly IBrush PlayerBrush = new SolidColorBrush(Color.Parse("#FFD54F"));
    private static readonly IBrush NpcBrush = new SolidColorBrush(Color.Parse("#8D8D8D"));

    public ObservableCollection<FfxiChatLine> ChatLines { get; } = [];
    public ObservableCollection<EntityDot> Dots { get; } = [];

    [ObservableProperty]
    public partial string Input { get; set; } = "";

    [ObservableProperty]
    public partial string CharacterName { get; set; } = "";

    [ObservableProperty]
    public partial string PositionText { get; set; } = "";

    [ObservableProperty]
    public partial int PlayerCount { get; set; }

    [ObservableProperty]
    public partial int NpcCount { get; set; }

    public GameViewModel(MainViewModel shell)
    {
        _shell = shell;

        FfxiZoneLoginReply? state = shell.Session.ZoneState;
        CharacterName = state?.Name ?? shell.Session.Handoff?.CharacterName ?? "";
        PositionText = state is null ? "" : $"x {state.X:F1}  y {state.Vertical:F1}  z {state.Depth:F1}  zone {state.ZoneNo}";

        shell.Session.ChatReceived += OnChat;
        shell.Session.EntitiesChanged += OnEntities;
    }

    private void OnChat(FfxiChatLine line) => Dispatcher.UIThread.Post(() =>
    {
        ChatLines.Add(line);

        // Keep the log bounded; a busy zone would otherwise grow it forever.
        while (ChatLines.Count > 500)
        {
            ChatLines.RemoveAt(0);
        }
    });

    private void OnEntities(IReadOnlyList<FfxiEntityUpdate> entities) => Dispatcher.UIThread.Post(() =>
    {
        RefreshRadar();
        return;
#pragma warning disable CS0162
        FfxiGameSession session = _shell.Session;
        if (session.ZoneState is null)
        {
            return;
        }

        // Centre on where we are *now*, not where we logged in - otherwise
        // walking slides the whole world off the edge of the radar.
        float centreX = session.PosX;
        float centreZ = session.PosDepth;

        Dots.Clear();
        int players = 0, npcs = 0;

        // Us, always dead centre.
        Dots.Add(new EntityDot(Radius - 4, Radius - 4, CharacterName, SelfBrush, 8));

        foreach (FfxiEntityUpdate entity in entities)
        {
            if (entity.UniqueNo == session.OwnCharId)
            {
                continue;
            }

            bool isPlayer = entity.PacketId == FfxiEntityUpdate.PlayerPacketId;
            if (isPlayer)
            {
                players++;
            }
            else
            {
                npcs++;
            }

            // Screen x follows game x; screen y follows game *depth*, not the
            // vertical axis - the radar is a top-down map, so height is
            // deliberately dropped rather than drawn.
            double dx = (entity.X - centreX) / RangeInUnits * Radius;
            double dz = (entity.Depth - centreZ) / RangeInUnits * Radius;

            if (Math.Abs(dx) > Radius || Math.Abs(dz) > Radius)
            {
                continue;
            }

            double size = isPlayer ? 8 : 5;
            Dots.Add(new EntityDot(
                Radius + dx - size / 2,
                Radius + dz - size / 2,
                isPlayer ? entity.Name ?? $"#{entity.UniqueNo}" : "",
                isPlayer ? PlayerBrush : NpcBrush,
                size));
        }

        PlayerCount = players;
        NpcCount = npcs;
#pragma warning restore CS0162
    });

    /// <summary>Map polygons, blips and route, refreshed together for the radar to draw.</summary>
    public event Action? RadarChanged;

    public IReadOnlyList<IReadOnlyList<(float X, float Depth)>> MapPolygons { get; private set; } = [];
    public IReadOnlyList<PortJeuno.App.Controls.RadarBlip> Blips { get; private set; } = [];
    public IReadOnlyList<(float X, float Depth)> RoutePoints { get; private set; } = [];

    public float CentreX => _shell.Session.PosX;
    public float CentreDepth => _shell.Session.PosDepth;

    private void RefreshRadar()
    {
        FfxiGameSession session = _shell.Session;

        MapPolygons = session.NavMesh?.WalkablePolygons(session.PosX, session.PosDepth, (float)RangeInUnits) ?? [];

        var blips = new List<PortJeuno.App.Controls.RadarBlip>
        {
            new(session.PosX, session.PosDepth, CharacterName, IsPlayer: true, IsSelf: true),
        };

        int players = 0, npcs = 0;
        foreach (FfxiEntityUpdate entity in session.KnownEntities())
        {
            if (entity.UniqueNo == session.OwnCharId)
            {
                continue;
            }

            bool isPlayer = entity.PacketId == FfxiEntityUpdate.PlayerPacketId;
            if (isPlayer) { players++; } else { npcs++; }

            blips.Add(new PortJeuno.App.Controls.RadarBlip(
                entity.X, entity.Depth, entity.Name ?? "", isPlayer, IsSelf: false));
        }

        Blips = blips;
        PlayerCount = players;
        NpcCount = npcs;

        RoutePoints = session.CurrentPath.Select(p => (p.X, p.Depth)).ToList();

        RadarChanged?.Invoke();
    }

    /// <summary>Walks to a spot on the map, routed around walls by the navmesh.</summary>
    public void WalkTo(float x, float depth) => _ = WalkToAsync(x, depth);

    private async Task WalkToAsync(float x, float depth)
    {
        await _shell.Session.WalkToAsync(x, depth);
        RefreshPosition();
    }

    [RelayCommand]
    private void StopWalking() => _shell.Session.StopWalking();

    [RelayCommand]
    private async Task SendAsync()
    {
        string text = Input.Trim();
        if (text.Length == 0)
        {
            return;
        }

        Input = "";

        // "/tell name message" is the one form worth special-casing; anything
        // starting with '!' is a GM command, which the server routes off the
        // ordinary say path, so it needs no handling here.
        if (text.StartsWith("/tell ", StringComparison.OrdinalIgnoreCase))
        {
            string[] parts = text[6..].Split(' ', 2);
            if (parts.Length == 2)
            {
                await _shell.Session.TellAsync(parts[0], parts[1]);
                ChatLines.Add(new FfxiChatLine(DateTimeOffset.Now, FfxiChatMessageType.Tell, ">> " + parts[0], parts[1]));
            }
            return;
        }

        // Echo locally: the server does not send our own say back to us, so
        // without this the chat box stays empty no matter how much we talk.
        ChatLines.Add(new FfxiChatLine(DateTimeOffset.Now, FfxiChatMessageType.Say, CharacterName, text));
        await _shell.Session.SayAsync(text);
    }

    /// <summary>How far one key press moves the character, in game units.</summary>
    [ObservableProperty]
    public partial double StepSize { get; set; } = 1.0;

    /// <summary>
    /// Steps the character. Screen-style directions: north decreases depth,
    /// east increases x - matching how the radar is drawn, so what you press
    /// matches what you see move.
    /// </summary>
    public void Move(string direction)
    {
        float step = (float)StepSize;
        (float dx, float dz) = direction switch
        {
            // North increases depth: the game compass and the depth axis run the
            // same way, which is the opposite of screen-space intuition.
            "north" => (0f, step),
            "south" => (0f, -step),
            "west" => (-step, 0f),
            "east" => (step, 0f),
            _ => (0f, 0f),
        };

        _shell.Session.Move(dx, dz);
        RefreshPosition();
        RefreshRadar();

        if (_shell.Session.LastMoveBlocked)
        {
            _shell.Status = "Blocked - that way is off the walkable surface.";
        }
    }

    private void RefreshPosition()
    {
        FfxiGameSession s = _shell.Session;
        PositionText = $"x {s.PosX:F1}  y {s.PosVertical:F1}  z {s.PosDepth:F1}  facing {s.Facing}";
    }

    [RelayCommand]
    private void MoveNorth() => Move("north");

    [RelayCommand]
    private void MoveSouth() => Move("south");

    [RelayCommand]
    private void MoveWest() => Move("west");

    [RelayCommand]
    private void MoveEast() => Move("east");

    [RelayCommand]
    private async Task LogoutAsync()
    {
        await _shell.Session.LogoutAsync();
        _shell.Navigate(new LoginViewModel(_shell));
    }
}
