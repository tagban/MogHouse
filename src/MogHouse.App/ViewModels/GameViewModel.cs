using System;
using System.Linq;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Media;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.App.ViewModels;

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

        // Dying, and being offered a way back up. Both are pushed at the
        // renderer rather than shown here: the box belongs in the window the
        // character is lying in, not in a panel beside it.
        // The zone's tune, and whatever the server changes it to.
        shell.Session.MusicChanged += OnMusicChanged;

        // Arriving somewhere else entirely - a zone line, !zone, a homepoint
        // return. The world window is built around one zone's geometry and
        // cannot be told to become another, so it is replaced.
        shell.Session.ZoneChanged += OnZoneChanged;

        // A GM teleport, a homepoint return, anything the server decides.
        // Without this the character stays where the renderer last drew them.
        shell.Session.MovedByServer += OnMovedByServer;

        shell.Session.DeathChanged += OnDeathChanged;
        shell.Session.RaiseOfferChanged += OnRaiseOfferChanged;

        OpenWorld();

        // A watchdog, because there is one state this client must never reach:
        // no window on screen at all.
        //
        // The launcher hides itself while the world window is up, so anything
        // that loses the world without saying so - a zone change that fails to
        // reopen, a renderer that dies, a path nobody has thought of - leaves a
        // running, logged-in, invisible client. That has happened twice and
        // both times it read as the client closing.
        //
        // This does not fix whatever went wrong. It makes sure you can see that
        // something did.
        _watchdog = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _watchdog.Tick += (_, _) =>
        {
            if (!IsZoning && _world is null or { Closed: true })
            {
                WorldVisibilityChanged?.Invoke(false);
            }
        };
        _watchdog.Start();
    }

    private DispatcherTimer? _watchdog;

    [ObservableProperty]
    public partial string WorldStatus { get; set; } = "Opening...";

    private LiveRadar? _world;

    /// <summary>
    /// Raised when the world window opens and again when it closes.
    ///
    /// The world is a native window of its own, beside the launcher rather
    /// than inside it. Left alone that means two windows from character select
    /// onwards - the world in front and the launcher behind it, holding the
    /// chat log the player actually wants. So the launcher steps aside while
    /// the world is up and comes back when it closes, and the chat goes with
    /// it. What the player sees is one window that becomes the game.
    /// </summary>
    public event Action<bool>? WorldVisibilityChanged;

    /// <summary>
    /// Whether the world window is up right now.
    ///
    /// The constructor opens the world, so the first
    /// <see cref="WorldVisibilityChanged"/> is raised before anything can have
    /// subscribed - the window is only handed this view model once
    /// construction has returned. Whoever attaches has to ask.
    /// </summary>
    public bool WorldIsOpen => _world is { Closed: false };
    private readonly FfxiEntityTracker _tracker = new();
    private CancellationTokenSource? _feeding;

    /// <summary>
    /// Opens the actual client - the zone, the character, everyone else in it -
    /// beside this window.
    ///
    /// The dots panel here was a stand-in from before there was a renderer to
    /// open. It stays because it is a useful thing to glance at while the
    /// window has focus, but it is not the game.
    /// </summary>
    private void OpenWorld()
    {
        FfxiGameSession session = _shell.Session;
        if (session.ZoneState is null)
        {
            return;
        }

        _tracker.SelfUniqueNo = session.ZoneState.UniqueNo;

        FfxiCharacter? self = _shell.SelectedCharacter;
        string look = self is null
            ? FfxiAppearance.LookString(1, 0)
            : FfxiAppearance.LookString(self.Race, self.Face);

        _openZone = session.ZoneState.ZoneNo;
        _world = LiveRadar.Open((int)session.ZoneState.ZoneNo, session.PosX, session.PosVertical, session.PosDepth,
                                session.ZoneState.GameTime, CharacterName, look);
        if (_world is null)
        {
            WorldStatus = "Could not open the world window. Check MOGHOUSE_FFXI_KEYTABLE and that the game files are where the client expects.";
            _shell.Status = WorldStatus;
            return;
        }

        WorldStatus = $"Open, in zone {session.ZoneState.ZoneNo}.";

        // Everything already said goes across, so the log does not restart at
        // whatever happens to arrive next.
        foreach (FfxiChatLine earlier in ChatLines)
        {
            _world.Say(earlier.Sender, earlier.Text);
        }
        WorldVisibilityChanged?.Invoke(true);

        // A window opened over a corpse. The health packet arrives in the
        // burst the server sends at zone-in, long before this window existed,
        // so someone who logs in dead - or reopens the world after closing it -
        // gets no event to tell them there is a way up.
        _world.ShowDeath(session.IsDead, session.HasRaiseOffer);

        // What the last session left behind, before anything else: the
        // volume has to be right before the first note rather than after it.
        _world.ShowSettings(MogHouseSettings.Current);

        // The world window is new; it has not heard the zone's music yet.
        OnMusicChanged(session.CurrentTrack);

        // The renderer owns movement and reports where it ended up; the session
        // owns everything the server has to be told about it.
        _feeding = new CancellationTokenSource();
        CancellationToken token = _feeding.Token;
        _ = Task.Run(async () =>
        {
            while (!token.IsCancellationRequested && _world is { Closed: false })
            {
                // Not while a zone is being read. Until it finishes the window
                // is still holding the position it had in the zone being left,
                // and telling the server that is how one zone change becomes
                // several: a position from the old zone can be standing in the
                // line that sent us here.
                if (_world.IsLoading != true &&
                    _world.Position() is (float x, float vertical, float depth, sbyte direction))
                {
                    session.PlaceAt(x, vertical, depth, direction);
                }

                while (_world?.TakeChat() is { Length: > 0 } typed)
                {
                    await TypedAsync(typed);
                }

                // Space: a jump on your feet, a wave lying down - see
                // JumpAsync. A corpse has no other way to be noticed.
                if (_world?.TakeJump() == true)
                {
                    await session.JumpAsync();
                }

                // Whichever button a dead character pressed. The renderer drew
                // the choice; only this half has a socket to say it down.
                switch (_world?.TakeDeathChoice())
                {
                    case NativeDeathChoice.HomePoint:
                        await session.ReturnToHomePointAsync();
                        break;

                    case NativeDeathChoice.AcceptRaise:
                        await session.AcceptRaiseAsync();
                        break;
                }

                // Volume and the minimap are changed by keys in the world
                // window, so this side only hears about it afterwards - and
                // writes it out, so the next session starts where this one
                // left off.
                if (_world?.TakeSettings() is { } changed)
                {
                    MogHouseSettings settings = MogHouseSettings.Current;
                    settings.MusicVolume = changed.MusicVolume;
                    settings.RadarTurnsWithPlayer = changed.RadarTurns;
                    settings.Save();
                }

                // Somewhere to send a bug from inside the world. The window
                // knows a corner was clicked; opening a browser is this side's
                // job, and works the same on three operating systems.
                switch (_world?.TakeLink())
                {
                    case NativeLink.Discord:
                        Links.Open(Links.Discord);
                        break;

                    case NativeLink.Issues:
                        Links.Open(Links.Issues);
                        break;
                }

                // Pushed rather than raised on change: hit points move
                // constantly, and the panel wants the current number, not a
                // notification that it moved.
                if (session.Health is { } vitals)
                {
                    _world?.ShowVitals(vitals);
                }

                _world?.Publish(_tracker);
                await Task.Delay(50, token).ConfigureAwait(false);
            }

            // Closing the world window is leaving, not a glitch to recover
            // from - say so rather than looking like nothing happened.
            //
            // And bring the launcher back. It hides itself while the world is
            // up, so without this the world closing leaves nothing on screen
            // at all: the app is still running, still logged in, and entirely
            // invisible. Which looks exactly like logging out having failed.
            if (!token.IsCancellationRequested)
            {
                Dispatcher.UIThread.Post(() =>
                {
                    WorldStatus = "Closed. Reopen it, or leave.";
                    WorldVisibilityChanged?.Invoke(false);
                });
            }
        }, token);
    }

    /// <summary>Opens it again after it has been closed.</summary>
    [RelayCommand]
    private void ReopenWorld()
    {
        if (_world is { Closed: false })
        {
            return;
        }

        _feeding?.Cancel();
        _world?.Dispose();
        _world = null;
        WorldVisibilityChanged?.Invoke(false);
        WorldStatus = "Opening...";
        OpenWorld();
    }

    /// <summary>Closes the world and goes back to the character list.</summary>
    [RelayCommand]
    private async Task LeaveAsync()
    {
        // Ask first, tear down after. REQLOGOUT is a request: LandSandBoat
        // runs a five second Leavegame effect and only then clears the
        // session, and LogoutAsync waits that out. Stopping the loop that
        // answers the server before asking meant nothing was left to finish
        // the handshake, so the logout never completed and the character sat
        // on the server until it was reaped about a minute later - which from
        // the outside is being unable to log back in.
        _shell.Status = "Leaving...";
        await _shell.Session.LogoutAsync();

        _watchdog?.Stop();
        _feeding?.Cancel();
        _world?.Dispose();
        _world = null;
        WorldVisibilityChanged?.Invoke(false);
        _shell.Status = "Left the world.";
    }

    /// <summary>
    /// Dying, and getting up again. The box is the renderer's to draw and the
    /// session's to fill in - the renderer knows where the body is and nothing
    /// about the state of it.
    /// </summary>
    private void OnDeathChanged(bool dead) =>
        _world?.ShowDeath(dead, dead && _shell.Session.HasRaiseOffer);

    /// <summary>Someone has cast Raise, which is what lights the second button.</summary>
    private void OnRaiseOfferChanged(bool offered) =>
        _world?.ShowDeath(_shell.Session.IsDead, offered);

    /// <summary>Which zone the open window was built for.</summary>
    private uint _openZone;

    /// <summary>
    /// How long a loading screen stays up however fast the zone reads.
    ///
    /// Zero. Swapping the zone inside the live window turned out to be quick
    /// enough that a loading screen had nothing to cover - it was a delay
    /// pretending to be work. The loading *state* is still tracked, because
    /// position reporting has to stay quiet while the zone is being replaced,
    /// but nothing waits on it any more.
    /// </summary>
    private static readonly TimeSpan MinimumLoadingScreen = TimeSpan.Zero;

    /// <summary>Waits for the zone to land, and for the floor to pass.</summary>
    private async Task HoldLoadingScreen()
    {
        DateTimeOffset started = DateTimeOffset.UtcNow;

        // The renderer reads the zone on its own thread, so this is how long it
        // actually took rather than how long it was expected to take.
        while (_world?.IsLoading == true && DateTimeOffset.UtcNow - started < TimeSpan.FromSeconds(60))
        {
            await Task.Delay(50);
        }

        TimeSpan left = MinimumLoadingScreen - (DateTimeOffset.UtcNow - started);
        if (left > TimeSpan.Zero)
        {
            await Task.Delay(left);
        }
    }

    /// <summary>Whether a zone is being loaded, and which one.</summary>
    [ObservableProperty]
    public partial bool IsZoning { get; set; }

    [ObservableProperty]
    public partial string ZoningTo { get; set; } = "";

    /// <summary>
    /// Rebuilds the world window on the other side of a zone change.
    ///
    /// Nothing here can be reused: the geometry, the collision, the baked map
    /// and the zone lines all belong to the zone being left. Without this the
    /// window carried on drawing the old zone while the server had already
    /// moved the character, so !zone and a walked zone line both looked like
    /// the client had frozen - the character could not move, because the
    /// ground under them was somewhere else entirely.
    /// </summary>
    private void OnZoneChanged(uint zone) => Dispatcher.UIThread.Post(async () =>
    {
        if (zone == _openZone)
        {
            // A teleport inside the zone you are already standing in still
            // arrives as a zone change: !goto and !bring both send you through
            // the zone server even when the destination is the zone you are
            // already in. There is nothing to load, but there is somewhere new
            // to stand, and returning here without doing anything left the
            // character exactly where they were - which is what made those two
            // commands look like they did nothing at all.
            //
            // The entities are re-sent on the way in, the same as any zone
            // change, so the old ones go.
            _tracker.Clear();
            FfxiGameSession here = _shell.Session;
            _world?.PlaceCharacter(here.PosX, here.PosVertical, here.PosDepth, here.Facing);
            Console.WriteLine(
                $"moved inside zone {zone} to {here.PosX:F1} {here.PosVertical:F1} {here.PosDepth:F1}");
            return;
        }

        ZoningTo = FfxiZoneNames.Get(zone)?.Replace('_', ' ') ?? $"zone {zone}";
        Console.WriteLine($"zoning to {zone} ({ZoningTo})");
        IsZoning = true;

        // The launcher is deliberately not shown. The world window draws its
        // own loading screen now, and swapping to another window and back is
        // the flicker keeping the window alive was meant to remove. It is only
        // brought back below, if there turns out to be no world window to zone.

        // Target indices are only unique within a zone, so carrying one across
        // would put an old entity's name and kind on whatever now holds that
        // index.
        _tracker.Clear();
        WorldStatus = $"Zoning to {ZoningTo}...";

        // The window draws the new zone itself rather than being replaced by
        // one that does. Closing it was what made zoning look like the client
        // shutting down: for as long as the new zone took to read there was
        // nothing on screen at all.
        FfxiGameSession session = _shell.Session;

        // Why, when it declines. Falling back opens a second window and closes
        // the first, which is the behaviour this replaced - so it should be
        // obvious that it happened rather than something to deduce from the
        // windows.
        if (_world is null)
        {
            Console.WriteLine("  no world window to zone; opening one");
        }
        else if (_world.Closed)
        {
            Console.WriteLine("  the world window had already closed; opening another");
        }

        if (_world is { Closed: false } &&
            _world.LoadZone((int)zone, ZoningTo, session.PosX, session.PosVertical, session.PosDepth, session.Facing))
        {
            _openZone = zone;

            // Held until the renderer says it has finished, and for a moment
            // beyond that.
            //
            // The floor is deliberate. Reading a zone is quick enough on a warm
            // disk that the screen can come and go faster than it can be read,
            // which looks like nothing happened - and "nothing happened" is
            // exactly what a zone change used to look like when it was broken.
            // A loading screen nobody sees cannot be trusted.
            await HoldLoadingScreen();

            IsZoning = false;
            _world.ShowZoneLines(session.ZoneLines);
            OnMusicChanged(session.CurrentTrack);
            WorldStatus = $"In {ZoningTo}.";
            return;
        }

        // No window to reuse, or no DAT for that zone: fall back to opening a
        // fresh one, and say so if even that fails. This is the only path that
        // needs the launcher, because it is the only one with nothing to look
        // at while it works.
        WorldVisibilityChanged?.Invoke(false);
        _feeding?.Cancel();
        _world?.Dispose();
        _world = null;
        OpenWorld();

        IsZoning = false;
        if (_world is null)
        {
            WorldStatus = $"Could not open {ZoningTo}. Reopen the world to try again.";
            _shell.Status = WorldStatus;
        }
    });

    /// <summary>
    /// The server sends a track number; the file it names has to be found.
    ///
    /// Which sound directory holds it is not fixed - the install spreads them
    /// across sound, sound2 and so on as expansions were added - so this looks
    /// rather than computes, and a track this install does not have goes
    /// quiet rather than failing.
    /// </summary>
    private void OnMusicChanged(int track) => Dispatcher.UIThread.Post(() =>
    {
        string? path = FfxiMusicFile.Resolve(FfxiInstall.Find() ?? "", track);
        if (path is null && track > 0)
        {
            _shell.Status = $"No file for music track {track}.";
        }
        _world?.ShowMusic(path);
    });

    private void OnMovedByServer() => Dispatcher.UIThread.Post(() =>
    {
        FfxiGameSession session = _shell.Session;
        _world?.PlaceCharacter(session.PosX, session.PosVertical, session.PosDepth, session.Facing);
        PositionText = $"x {session.PosX:F1}  y {session.PosVertical:F1}  z {session.PosDepth:F1}";
    });

    private void OnChat(FfxiChatLine line) => Dispatcher.UIThread.Post(() =>
    {
        ChatLines.Add(line);

        // The world window keeps its own log and draws it in front of the
        // player. Without this the launcher has the conversation and the
        // window the player is looking at has none.
        _world?.Say(line.Sender, line.Text);

        // Keep the log bounded; a busy zone would otherwise grow it forever.
        while (ChatLines.Count > 500)
        {
            ChatLines.RemoveAt(0);
        }
    });

    private void OnEntities(IReadOnlyList<FfxiEntityUpdate> entities) => Dispatcher.UIThread.Post(() =>
    {
        // The tracker is what the renderer draws from: it holds what an entity
        // looks like, whether the server means it to be seen, and which fields
        // a partial update may change.
        DateTimeOffset now = DateTimeOffset.UtcNow;
        foreach (FfxiEntityUpdate update in entities)
        {
            _tracker.Observe(update, now);
        }

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
    public IReadOnlyList<MogHouse.App.Controls.RadarBlip> Blips { get; private set; } = [];
    public IReadOnlyList<(float X, float Depth)> RoutePoints { get; private set; } = [];

    public float CentreX => _shell.Session.PosX;
    public float CentreDepth => _shell.Session.PosDepth;

    private void RefreshRadar()
    {
        FfxiGameSession session = _shell.Session;

        MapPolygons = session.NavMesh?.WalkablePolygons(session.PosX, session.PosDepth, (float)RangeInUnits) ?? [];

        var blips = new List<MogHouse.App.Controls.RadarBlip>
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

            blips.Add(new MogHouse.App.Controls.RadarBlip(
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
        await TypedAsync(text);
    }

    /// <summary>
    /// What a typed line means, wherever it was typed.
    ///
    /// The world window and the box in this panel both end up here, and the
    /// client's own commands are answered rather than said: typing /logout was
    /// broadcast to the zone as the words "/logout" and did nothing else,
    /// because only the console client ever ran a line past the parser.
    /// </summary>
    private async Task TypedAsync(string text)
    {
        FfxiClientCommand command = FfxiClientCommands.Parse(text);
        switch (command.Kind)
        {
            case FfxiClientCommandKind.Logout:
                await LeaveAsync();
                return;

            case FfxiClientCommandKind.Shutdown:
                await _shell.Session.LogoutAsync(FfxiLogoutKind.Shutdown);
                _shell.Status = "Shut down.";
                return;

            case FfxiClientCommandKind.HomePoint:
                await _shell.Session.ReturnToHomePointAsync();
                return;

            // Every channel the real client answers to, and the short forms
            // nobody types the long version of: /s /sh /y /p /l /ls /l2 /u /em.
            case FfxiClientCommandKind.Chat:
                ChatLines.Add(new FfxiChatLine(DateTimeOffset.Now, ToMessageType(command.Channel),
                                               CharacterName, command.Rest));
                _world?.Say(CharacterName, command.Rest);
                await _shell.Session.SayAsync(command.Rest, command.Channel);
                return;

            case FfxiClientCommandKind.Tell:
                await _shell.Session.TellAsync(command.Recipient, command.Rest);
                // A tell is not echoed back to whoever sent it, so without this
                // the only evidence it went anywhere is the reply.
                Note(FfxiChatMessageType.Tell, ">> " + command.Recipient, command.Rest);
                return;

            case FfxiClientCommandKind.Incomplete:
                Note(FfxiChatMessageType.System1, "", $"/{command.Name} needs more than that.");
                return;

            case FfxiClientCommandKind.Unsupported:
                Note(FfxiChatMessageType.System1, "", $"/{command.Name} is not something this client does yet.");
                return;

            // /talk is deliberately not here. It needs a target, and in this
            // window you get one by clicking an NPC, which already talks to it.
        }

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

    /// <summary>Says something in both places a player might be looking.</summary>
    private void Note(FfxiChatMessageType kind, string who, string what)
    {
        ChatLines.Add(new FfxiChatLine(DateTimeOffset.Now, kind, who, what));
        _world?.Say(who, what);
    }

    /// <summary>Which colour the log gives a channel.</summary>
    private static FfxiChatMessageType ToMessageType(FfxiChatKind channel) => channel switch
    {
        // Yell has no type of its own in the log; it is a shout that carries
        // further, and the server sends it back as one.
        FfxiChatKind.Shout or FfxiChatKind.Yell => FfxiChatMessageType.Shout,
        FfxiChatKind.Party => FfxiChatMessageType.Party,
        FfxiChatKind.Linkshell1 or FfxiChatKind.Linkshell2 => FfxiChatMessageType.Linkshell,
        FfxiChatKind.Emote => FfxiChatMessageType.Emotion,
        _ => FfxiChatMessageType.Say,
    };

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
