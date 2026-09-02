using MogHouse.Core.Interop;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// The renderer, opened beside a live session and fed from its entity tracker.
///
/// The viewer owns a window and an event loop, so it gets a thread of its own
/// and the session keeps this one. Publishing is a copy under no lock beyond
/// the one inside the native link - the list is small and replaced whole.
/// </summary>
public sealed class LiveRadar : IDisposable
{
    private readonly NativeViewer _viewer;
    private readonly Thread? _thread;
    private bool _closed;

    /// <param name="ownThread">
    /// Whether to start the render loop on a thread of its own.
    ///
    /// True is what this has always done, and it is wrong on macOS: the loop
    /// creates the window, and AppKit refuses to make an NSWindow anywhere but
    /// the main thread - SDL reports it as "No available video device", which
    /// reads like a driver fault and is not one. Windows has no such rule,
    /// which is why it went unnoticed.
    ///
    /// False leaves the loop unstarted so the caller can run it itself with
    /// <see cref="Run"/>, from whichever thread it knows to be the main one.
    /// </param>
    private LiveRadar(NativeViewer viewer, bool ownThread)
    {
        _viewer = viewer;
        if (ownThread)
        {
            _thread = new Thread(() => _viewer.Run()) { IsBackground = true, Name = "moghouse-renderer" };
            _thread.Start();
        }
    }

    /// <summary>
    /// Runs the render loop on the calling thread, blocking until the window
    /// closes. Only for a radar opened with <c>ownThread: false</c>; one opened
    /// the old way is already running on a thread of its own.
    /// </summary>
    public int Run()
    {
        if (_thread is not null)
        {
            throw new InvalidOperationException(
                "This LiveRadar already runs on its own thread; open it with ownThread: false to run it here.");
        }

        try
        {
            return _viewer.Run();
        }
        finally
        {
            // Run returning is the window closing, and it is the only signal
            // there is when no thread is being watched for.
            _finished = true;
        }
    }

    /// <summary>Set when <see cref="Run"/> returns, for a radar with no thread of its own.</summary>
    private bool _finished;

    /// <summary>
    /// Whether the window has gone. Run() owns the event loop and returns when
    /// it closes, so the thread ending is the window closing.
    ///
    /// Someone shutting the window is them ending the session, and nothing was
    /// watching for it: the window went and the session held on for the rest of
    /// its time, skipping the logout it would have done on the way out.
    ///
    /// With no thread of its own there is nothing to test for life, so the
    /// answer comes from Run having returned instead. Reading _thread directly
    /// here would throw the moment a main-thread radar was asked whether it had
    /// closed.
    /// </summary>
    public bool Closed => _thread is not null ? !_thread.IsAlive : _finished;

    /// <summary>
    /// Opens the zone the character is actually standing in, at the position
    /// the server reported. Returns null, with a reason, if anything needed is
    /// missing - a radar is not worth failing a login over.
    /// </summary>
    /// <summary>
    /// Where a key table lives: what the environment says, or beside the
    /// executable. An empty string means "not found", which the renderer
    /// already reports for itself.
    /// </summary>
    private static string KeyTable(string variable, string fileName)
    {
        if (Environment.GetEnvironmentVariable(variable) is { Length: > 0 } configured)
        {
            return configured;
        }

        foreach (string folder in new[] { "keys", Path.Combine("data", "keys") })
        {
            string beside = Path.Combine(AppContext.BaseDirectory, folder, fileName);
            if (File.Exists(beside))
            {
                return beside;
            }
        }

        return "";
    }

    public static LiveRadar? Open(int zoneId, float x, float vertical, float depth, uint serverClock = 0,
                                  string? playerName = null, string? playerLook = null, bool ownThread = true)
    {
        // Beside the executable when nothing points elsewhere, so a copied
        // folder runs with no environment set - the same portability the
        // compression tables, the glyph atlas and the water already have. A
        // development checkout keeps these in keys/ at the repository root and
        // a packaged build puts them in keys/ next to the exe, so one relative
        // path serves both.
        string keys = KeyTable("MOGHOUSE_FFXI_KEYTABLE", "mzb_key_table.bin");
        string keys2 = KeyTable("MOGHOUSE_FFXI_KEYTABLE2", "mmb_key_table2.bin");
        if (keys.Length == 0)
        {
            Console.WriteLine("--view needs MOGHOUSE_FFXI_KEYTABLE; run tools/keytables.py");
            return null;
        }

        string? zonePath;
        try
        {
            var table = new FfxiFileTable(FfxiFileTable.DefaultInstallRoot());
            zonePath = table.ZonePath(zoneId);
        }
        catch (Exception error)
        {
            Console.WriteLine($"--view could not read the file table: {error.Message}");
            return null;
        }

        if (zonePath is null || !File.Exists(zonePath))
        {
            Console.WriteLine($"--view: zone {zoneId} is not installed");
            return null;
        }

        Console.WriteLine($"--view: opening zone {zoneId} from {zonePath}");

        var options = new NativeViewerOptions
        {
            ZonePath = zonePath,
            KeyTablePath = keys,
            KeyTable2Path = keys2,
            // Who we actually are, when the caller knows. The fallback is a
            // hume male, which is what every character used to be regardless of
            // what the roster said they were.
            Look = playerLook ?? Environment.GetEnvironmentVariable("MOGHOUSE_LOOK") ?? "1,0,0,1,1,1,1",
            // The server's own clock, so this client and a retail one
            // side by side show the same hour and the same light.
            ServerClock = serverClock,
            // The renderer works in Y-up. Turning FFXI's Y-down frame the
            // right way up is a half turn about X, so the depth axis flips
            // along with the vertical - see renderer/zonemesh.cpp.
            //
            // Negating the vertical alone looked right for a long time, because
            // the world was built the same wrong way and the two agreed with
            // each other. The radar dots landing where they should proved only
            // that; a mirror is self-consistent.
            CharacterAt = string.Create(System.Globalization.CultureInfo.InvariantCulture,
                $"{x},{-vertical},{-depth}"),

            // Set MOGHOUSE_SCREENSHOT to check the live radar without watching
            // it. The wait is in frames, and it has to outlast the first few
            // entity updates or the shot shows an empty radar.
            ZoneName = FfxiZoneNames.Get((uint)zoneId) ?? $"ZONE {zoneId}",
            PlayerName = playerName,
            ScreenshotPath = Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT"),
            ScreenshotAfterFrames =
                int.TryParse(Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT_AFTER"), out int after)
                    ? after
                    : 0,
        };

        return new LiveRadar(new NativeViewer(options), ownThread);
    }

    /// <summary>
    /// Why a zone could not be drawn, or null when one could.
    ///
    /// <para>
    /// Worth asking before it matters. <see cref="Open"/> refuses to open at
    /// all without these and says so, but a window opened by
    /// <see cref="OpenEmpty"/> comes up perfectly well and only fails later,
    /// when a zone is asked for - and it fails inside the renderer, whose
    /// complaint goes to its own log. What the player sees is a sign-in that
    /// works followed by a black world, which is a miserable way to be told
    /// that a 256-byte file is missing.
    /// </para>
    /// </summary>
    public static string? MissingZoneKeys() =>
        KeyTable("MOGHOUSE_FFXI_KEYTABLE", "mzb_key_table.bin").Length == 0
            ? "The MZB key table was not found. It should be in keys/ beside the client, " +
              "or named by MOGHOUSE_FFXI_KEYTABLE; tools/keytables.py builds one."
            : null;

    /// <summary>
    /// Opens the window with nothing in it, for the screens that come before
    /// there is a character standing anywhere - signing in, picking who to
    /// play. A zone arrives later through <see cref="LoadZone"/>, which is the
    /// same path zoning between areas already takes.
    ///
    /// <para>
    /// This is what lets the client draw its own screens instead of putting a
    /// separate toolkit's window in front of the game. The screens are forms in
    /// the renderer, so the world can be behind them rather than replaced by
    /// them.
    /// </para>
    /// </summary>
    /// <param name="ownThread">
    /// As <see cref="Open"/>. False is right for the client, where the main
    /// thread runs the loop because AppKit insists on it.
    /// </param>
    /// <summary>
    /// The zone drawn behind the client's own screens.
    ///
    /// Zone 0 - "selp", the remnants of Sel Phiner - which is the backdrop the
    /// retail client stands its characters in. It exists for nothing else: a
    /// green plain with cliffs around the edge and no reason to walk anywhere,
    /// which is why parts of it are missing and why that does not matter.
    ///
    /// MOGHOUSE_SCENE_ZONE picks another, and -1 opens onto nothing. Note that
    /// 0 is a real zone here rather than "none", which is exactly the trap this
    /// comment exists to point at.
    /// </summary>
    public const int DefaultSceneZone = 0;

    /// <summary>No scene at all - a plain black screen behind the forms.</summary>
    private const int NoSceneZone = -1;

    /// <summary>
    /// Which zone to open behind the screens: what the environment asks for,
    /// or Sel Phiner.
    /// </summary>
    private static int SceneZone() =>
        int.TryParse(Environment.GetEnvironmentVariable("MOGHOUSE_SCENE_ZONE"), out int wanted)
            ? wanted
            : DefaultSceneZone;

    public static LiveRadar OpenEmpty(bool ownThread = false)
    {
        // A zone behind the sign-in, which is how the game has always looked
        // while it asks who you are.
        //
        // It is not only decoration. Several things are built once from the
        // zone that is present when the window opens - the radar's map among
        // them - and a window opened onto nothing has none of them for the rest
        // of the session. Opening onto a scene means the client is in its
        // normal state before anybody types anything.
        string scene = string.Empty;
        int sceneZone = SceneZone();
        if (sceneZone != NoSceneZone)
        {
            try
            {
                var table = new FfxiFileTable(FfxiFileTable.DefaultInstallRoot());
                if (table.ZonePath(sceneZone) is { } path && File.Exists(path))
                {
                    scene = path;
                }
            }
            catch (Exception)
            {
                // No scene is a plainer client, not a broken one.
            }
        }

        var options = new NativeViewerOptions
        {
            ZonePath = scene,
            KeyTablePath = KeyTable("MOGHOUSE_FFXI_KEYTABLE", "mzb_key_table.bin"),
            KeyTable2Path = KeyTable("MOGHOUSE_FFXI_KEYTABLE2", "mmb_key_table2.bin"),

            // No look, deliberately. The scene wants nobody standing in it, and
            // the character is built and uploaded when one is actually chosen.
            //
            // No name either: the backdrop is scenery, not somewhere the player
            // is, and labelling it in the corner of a sign-in screen only
            // invites the question of how to walk there. Sel Phiner has no name
            // worth showing in any case.
            ZoneName = string.Empty,

            // Holds the hour still, as the standalone viewer's own
            // MOGHOUSE_TIME does. Worth having in the client too: the light a
            // zone is under is the hardest thing to compare between two runs
            // when it will not stop moving.
            TimeOfDay = int.TryParse(Environment.GetEnvironmentVariable("MOGHOUSE_TIME"), out int hhmm)
                ? hhmm
                : null,
            ScreenshotPath = Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT"),
            ScreenshotAfterFrames =
                int.TryParse(Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT_AFTER"), out int after)
                    ? after
                    : 0,
        };

        return new LiveRadar(new NativeViewer(options), ownThread);
    }

    /// <summary>
    /// Stands a set of characters up in the world for the player to choose
    /// between, and reports which one they pick.
    /// </summary>
    /// <param name="cast">
    /// Who to show, in the order they should stand. The id given to each is
    /// what comes back from <see cref="TakeTalk"/> when it is chosen, so a
    /// caller can use whatever numbering suits it.
    /// </param>
    /// <remarks>
    /// The positions are not given here. Standing people on the floor takes the
    /// zone's collision and framing them takes the camera, and both of those
    /// live in the renderer - so this says who, and the renderer says where.
    /// </remarks>
    public void ShowLineup(IReadOnlyList<(uint Id, string Name, ushort Race, ushort Face, int Style)> cast)
    {
        if (_closed)
        {
            return;
        }

        var entities = new NativeRadarEntity[cast.Count];
        for (int i = 0; i < cast.Count; i++)
        {
            NativeRadarEntity entity = new()
            {
                Id = cast[i].Id,

                // A player, so the character loader builds them out of a race
                // and equipment rather than looking for a creature model.
                Kind = (int)FfxiEntityKind.Player,

                // Only a triggerable entity answers a click, and answering a
                // click is the entire point of this one.
                Triggerable = 1,
                HealthPercent = -1,

                // 1 draws a pale blank shape, 2 draws them faded until pointed
                // at. See MhRadarEntity.silhouette.
                Silhouette = cast[i].Style,
            };
            entity.SetName(cast[i].Name);

            // Race and face are all the roster gives; the gear slots take the
            // same stand-in the player's own look uses, because the server
            // never sends us what our own characters are wearing.
            entity.SetLook(new FfxiEntityLook(
                FfxiLookKind.Equipped, ModelId: 0,
                Race: (byte)cast[i].Race, Face: (byte)cast[i].Face,
                Head: FfxiAppearance.UnknownGear, Body: FfxiAppearance.UnknownGear,
                Hands: FfxiAppearance.UnknownGear, Legs: FfxiAppearance.UnknownGear,
                Feet: FfxiAppearance.UnknownGear,
                Main: 0, Sub: 0, Ranged: 0));

            entities[i] = entity;
        }

        _viewer.SetEntities(entities);
        _viewer.SetLineup(true);
    }

    /// <summary>
    /// Whether the game's own furniture - the radar, the chat panel - is drawn.
    /// Off while the client is showing its own screens.
    /// </summary>
    public void ShowHud(bool on)
    {
        if (!_closed)
        {
            _viewer.ShowHud(on);
        }
    }

    /// <summary>Takes the line-up down, leaving the world as it was.</summary>
    public void HideLineup()
    {
        if (!_closed)
        {
            _viewer.SetLineup(false);
            _viewer.SetEntities([]);
        }
    }

    /// <summary>
    /// Tells the renderer who the player turned out to be - their name for the
    /// nameplate, and their look for the body, which is applied at the next
    /// zone load.
    /// </summary>
    public void SetPlayer(string? name = null, string? look = null)
    {
        if (!_closed)
        {
            _viewer.SetPlayer(name, look);
        }
    }

    /// <summary>
    /// Hands the renderer the server's clock, so the sky matches everyone
    /// else's. Known at zone-in, which is after the window opened.
    /// </summary>
    public void SetClock(uint serverClock)
    {
        if (!_closed)
        {
            _viewer.SetClock(serverClock);
        }
    }

    /// <summary>
    /// Puts a screen up, replacing whatever was showing. Taking it down is
    /// <see cref="HideForm"/>.
    /// </summary>
    public void ShowForm(string title, string message, IReadOnlyList<NativeFormRow> rows)
    {
        if (!_closed)
        {
            _viewer.ShowForm(title, message, rows);
        }
    }

    /// <summary>Takes the current screen down.</summary>
    public void HideForm()
    {
        if (!_closed)
        {
            _viewer.HideForm();
        }
    }

    /// <summary>
    /// What the player pressed, or null while they are still filling the screen
    /// in. Consumed, like <see cref="TakeChat"/>.
    /// </summary>
    public NativeFormResult? TakeFormResult() => _closed ? null : _viewer.TakeFormResult();

    /// <summary>
    /// Whether the player has asked to jump since this was last called.
    /// Consumed, so each jump reaches the server once.
    /// </summary>
    public bool TakeJump() => !_closed && _viewer.TakeJump();

    /// <summary>The next line the player typed, or null if they have not.</summary>
    public string? TakeChat() => _closed ? null : _viewer.TakeChat();

    /// <summary>
    /// Puts the character where the server says it is, converting out of the
    /// protocol's frame the same way <see cref="Position"/> converts into it.
    /// </summary>
    public void PlaceCharacter(float x, float vertical, float depth, sbyte direction)
    {
        if (_closed)
        {
            return;
        }

        // The half turn about X, and the heading convention the rest of this
        // agrees on: rotation zero faces +x, and the angle runs backwards.
        float heading = (float)(Math.PI / 2 - (direction & 0xFF) * (Math.PI * 2) / 256);
        _viewer.PlaceCharacter(x, -vertical, -depth, heading);
    }

    /// <summary>
    /// Where the renderer has walked the character, in the protocol's own
    /// terms: the inverse of the half turn about X that the world is built
    /// with, so both the vertical and the depth axis flip back, and direction
    /// is a byte over the full circle rather than radians.
    /// </summary>
    public (float X, float Vertical, float Depth, sbyte Direction)? Position()
    {
        if (_closed || !_viewer.TryGetCharacter(out float x, out float y, out float z, out float heading))
        {
            return null;
        }

        // The inverse of the import above, and wrong in the same way until
        // now. The half turn about X reverses which way a yaw goes, which pi -
        // h accounts for - but rotation zero faces +x rather than +z, and that
        // quarter turn was missing. Both halves being wrong together meant a
        // round trip through our own code agreed with itself, and only another
        // client could see it.
        double turns = (Math.PI / 2 - heading) / (Math.PI * 2);
        turns -= Math.Floor(turns);
        return (x, -y, -z, (sbyte)(byte)Math.Round(turns * 256));
    }

    /// <summary>
    /// Shows one chat line in the renderer's panel.
    /// </summary>
    /// <remarks>
    /// The panel's font has capitals, digits and a little punctuation and
    /// nothing else, so this does not try to preserve the text exactly - it is
    /// a monitor for whether data is flowing, not a chat client.
    /// </remarks>
    /// <summary>
    /// Where this zone's exits are, so the renderer can draw them.
    ///
    /// The same (x, -y, -z) the rest of the world uses: the zone line's
    /// vertical is where the player's feet will be, and the marker rises
    /// from there.
    /// </summary>
    public void ShowZoneLines(IReadOnlyList<FfxiZoneLine> lines)
    {
        var markers = new NativeZoneLine[lines.Count];
        for (int i = 0; i < lines.Count; i++)
        {
            markers[i] = new NativeZoneLine
            {
                X = lines[i].FromX,
                Y = -lines[i].FromVertical,
                Z = -lines[i].FromDepth,
                Radius = lines[i].Radius,
            };
        }

        _viewer.SetZoneLines(markers);
    }

    /// <summary>Who the player asked to talk to this frame, or 0.</summary>
    public uint TakeTalk() => _viewer.TakeTalk();

    /// <summary>
    /// Puts the death box up, or takes it down, and says whether its second
    /// button can be pressed.
    ///
    /// Both facts are the session's: hit points arrive in one packet and the
    /// raise offer in another, and the renderer sees neither. Pushed rather
    /// than polled, because there is nothing here for the renderer to poll.
    /// </summary>
    public void ShowDeath(bool dead, bool raiseOffered)
    {
        if (_closed)
        {
            return;
        }
        _viewer.SetDeath(dead, raiseOffered);
    }

    /// <summary>Whether the window is reading a zone right now.</summary>
    public bool IsLoading => !_closed && _viewer.IsLoading;

    /// <summary>
    /// Draws a different zone in the window already open.
    ///
    /// Returns false when this install has no DAT for that zone, which is the
    /// caller's cue to say so rather than to leave a window showing a zone the
    /// player has already left.
    /// </summary>
    public bool LoadZone(int zoneId, string zoneName, float x, float vertical, float depth, sbyte direction)
    {
        if (_closed)
        {
            return false;
        }

        string? path;
        try
        {
            path = new FfxiFileTable(FfxiFileTable.DefaultInstallRoot()).ZonePath(zoneId);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  cannot reach the file table for zone {zoneId}: {ex.Message}");
            return false;
        }
        if (path is null)
        {
            Console.WriteLine($"  this install has no DAT for zone {zoneId}");
            return false;
        }

        // The same half turn about X and heading convention PlaceCharacter
        // uses - see the note there.
        float heading = (float)(Math.PI / 2 - (direction & 0xFF) * (Math.PI * 2) / 256);
        _viewer.LoadZone(path, zoneName, x, -vertical, -depth, heading);
        return true;
    }

    /// <summary>Hands the saved preferences to the world window.</summary>
    public void ShowSettings(MogHouseSettings settings)
    {
        if (!_closed)
        {
            _viewer.SetSettings(settings.MusicVolume, settings.RadarTurnsWithPlayer);
        }
    }

    /// <summary>What the player changed in the world window, or null.</summary>
    public (float MusicVolume, bool RadarTurns)? TakeSettings() =>
        _closed ? null : _viewer.TakeSettings();

    /// <summary>The music file the zone wants, or null for silence.</summary>
    public void ShowMusic(string? path)
    {
        if (!_closed)
        {
            _viewer.SetMusic(path);
        }
    }

    /// <summary>Which link the player clicked in the world window, or None.</summary>
    public NativeLink TakeLink() => _closed ? NativeLink.None : _viewer.TakeLink();

    /// <summary>The player's own hit points, magic and TP, for the world window.</summary>
    public void ShowVitals(FfxiCharacterHealth health)
    {
        if (_closed)
        {
            return;
        }
        _viewer.SetVitals(health.Hp, health.Mp, health.Tp, health.HealthPercent, health.ManaPercent);
    }

    /// <summary>
    /// What the player pressed in that box, or None. Consumed, so a press
    /// reaches the server once.
    /// </summary>
    public NativeDeathChoice TakeDeathChoice() => _closed ? NativeDeathChoice.None : _viewer.TakeDeathChoice();

    public void Say(string? sender, string? text)
    {
        if (_closed)
        {
            return;
        }
        string line = string.IsNullOrEmpty(sender) ? (text ?? "") : $"{sender}: {text}";
        _viewer.PushChat(line);
    }

    /// <summary>Pushes what the tracker currently believes is nearby.</summary>
    public void Publish(FfxiEntityTracker tracker)
    {
        if (_closed)
        {
            return;
        }

        // Anything the server has hidden is left off entirely - no body, no
        // name, no dot. An auction counter is an NPC you talk to and never
        // see, and drawing one puts a hume male behind the counter.
        IReadOnlyList<FfxiTrackedEntity> visible =
            [.. tracker.Visible(DateTimeOffset.UtcNow).Where(e => !e.Hidden)];
        var entities = new NativeRadarEntity[visible.Count];
        for (int i = 0; i < visible.Count; i++)
        {
            NativeRadarEntity entity = new()
            {
                X = visible[i].X,
                // Depth flips into the renderer's frame, the same as the
                // character's own position - anything else puts the dots on
                // the wrong side of the map they are drawn over.
                Z = -visible[i].Depth,
                Y = -visible[i].Vertical,
                // Direction is a byte over the full circle. The half turn
                // about X reverses which way a yaw goes, so this carries the
                // same pi - h correction the player's own heading does.
                // Rotation zero faces +x in FFXI, and the angle runs the other
                // way round: the server builds it as
                // atan2(dz, dx) * -(128/pi) - see worldAngle in common/utils.cpp.
                // Through the half turn about X that builds this world, that
                // comes out as pi/2 - r, not pi - r. The quarter turn between
                // those two is why NPCs stood facing across their counters
                // instead of over them.
                Heading = (float)(Math.PI / 2 - (visible[i].Direction & 0xFF) * (Math.PI * 2) / 256),
                Kind = (int)visible[i].Kind,
                Id = visible[i].UniqueNo,
                NameHidden = visible[i].NameHidden ? 1 : 0,
            };
            entity.SetName(visible[i].Name);
            entity.SetLook(visible[i].Look);
            entity.GmLevel = visible[i].GmLevel;
            entity.HealthPercent = visible[i].HealthPercent ?? -1;
            entity.Triggerable = visible[i].Triggerable ? 1 : 0;
            entities[i] = entity;
        }
        _viewer.SetEntities(entities);
    }

    public void Dispose()
    {
        if (_closed)
        {
            return;
        }
        _closed = true;

        _viewer.Stop();
        _thread.Join(TimeSpan.FromSeconds(5));
        _viewer.Dispose();
    }
}
