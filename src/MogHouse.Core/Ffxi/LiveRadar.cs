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
    private readonly Thread _thread;
    private bool _closed;

    private LiveRadar(NativeViewer viewer)
    {
        _viewer = viewer;
        _thread = new Thread(() => _viewer.Run()) { IsBackground = true, Name = "moghouse-renderer" };
        _thread.Start();
    }

    /// <summary>
    /// Whether the window has gone. Run() owns the event loop and returns when
    /// it closes, so the thread ending is the window closing.
    ///
    /// Someone shutting the window is them ending the session, and nothing was
    /// watching for it: the window went and the session held on for the rest of
    /// its time, skipping the logout it would have done on the way out.
    /// </summary>
    public bool Closed => !_thread.IsAlive;

    /// <summary>
    /// Opens the zone the character is actually standing in, at the position
    /// the server reported. Returns null, with a reason, if anything needed is
    /// missing - a radar is not worth failing a login over.
    /// </summary>
    public static LiveRadar? Open(int zoneId, float x, float vertical, float depth, uint serverClock = 0,
                                  string? playerName = null, string? playerLook = null)
    {
        string keys = Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_KEYTABLE") ?? "";
        string keys2 = Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_KEYTABLE2") ?? "";
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

        return new LiveRadar(new NativeViewer(options));
    }

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
