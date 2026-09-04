using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Everything that has to happen while somebody is in the world.
///
/// <para>
/// The renderer owns movement and reports where the character ended up; the
/// session owns everything the server has to be told about it. This is the
/// wiring between the two: what the player typed, what they jumped over, what
/// killed them, which zone they walked into, and what the server said back.
/// </para>
///
/// <para>
/// It lived in a view model, which is the wrong place for it - none of it draws
/// anything. Anything that did draw stayed behind, and what is here talks only
/// to the session and to the renderer.
/// </para>
/// </summary>
public sealed class WorldLoop
{
    private readonly FfxiGameSession _session;
    private readonly LiveRadar _world;
    private readonly FfxiEntityTracker _tracker;
    private readonly string _who;
    private readonly TextWriter _say;

    private uint _openZone;
    private bool _leaving;

    /// Whether the welcome popup is still up and waiting to be dismissed.
    private bool _welcoming;

    /// <param name="tracker">
    /// What the renderer draws entities from. Handed in rather than made here
    /// because whoever zoned in has already been feeding it.
    /// </param>
    /// <param name="who">
    /// The character's own name, for echoing back what they say. The session
    /// does not carry it - it knows the character it selected, not what to put
    /// in front of a line of chat.
    /// </param>
    public WorldLoop(FfxiGameSession session, LiveRadar world, FfxiEntityTracker tracker,
                     uint openZone, string who, TextWriter? say = null)
    {
        _session = session;
        _world = world;
        _tracker = tracker;
        _openZone = openZone;
        _who = who;
        _say = say ?? Console.Out;
    }

    /// <summary>
    /// Wires the session's events to the world and runs until the window
    /// closes. Blocking, and not on the thread drawing the world.
    /// </summary>
    public void Run()
    {
        Attach();

        // What the last session left behind, before anything else: the volume
        // has to be right before the first note rather than after it.
        _world.ShowSettings(MogHouseSettings.Current);

        // A window opened over a corpse. The health packet arrives in the burst
        // the server sends at zone-in, long before anyone was watching, so
        // somebody who logs in dead gets no event to tell them there is a way
        // up.
        _world.ShowDeath(_session.IsDead, _session.HasRaiseOffer);
        _world.ShowZoneLines(_session.ZoneLines);
        _world.ShowWeather(_session.CurrentWeather);
        PlayMusic(_session.CurrentTrack);
        Welcome();

        try
        {
            Pump();
        }
        finally
        {
            Detach();
        }
    }

    private void Attach()
    {
        _session.ChatReceived += OnChat;
        _session.EntitiesChanged += OnEntities;
        _session.ZoneChanged += OnZoneChanged;
        _session.MusicChanged += PlayMusic;
        _session.WeatherChanged += OnWeatherChanged;
        _session.MovedByServer += OnMovedByServer;
        _session.DeathChanged += OnDeathChanged;
        _session.RaiseOfferChanged += OnRaiseOfferChanged;
    }

    private void Detach()
    {
        _session.ChatReceived -= OnChat;
        _session.EntitiesChanged -= OnEntities;
        _session.ZoneChanged -= OnZoneChanged;
        _session.MusicChanged -= PlayMusic;
        _session.WeatherChanged -= OnWeatherChanged;
        _session.MovedByServer -= OnMovedByServer;
        _session.DeathChanged -= OnDeathChanged;
        _session.RaiseOfferChanged -= OnRaiseOfferChanged;
    }

    /// <summary>The loop proper, until the window goes.</summary>
    /// <summary>
    /// A word to testers as they arrive, because a reporting tool nobody knows
    /// about collects nothing. Shown as a popup rather than said into chat: the
    /// first thing on screen at zone-in is a burst of the server's own
    /// messages, and a line about /bug would scroll past behind them.
    /// </summary>
    private void Welcome()
    {
        _world.ShowForm("MOGHOUSE XI",
                        "If you find a bug, face it, and type /bug <insert context here> and hit enter.",
                        new[] { NativeFormRow.Button("Got it") });
        _welcoming = true;
    }

    private void Pump()
    {
        while (!_world.Closed && !_leaving)
        {
            // Dismissed. Only while the welcome is up, so this does not eat the
            // result of any other form the world puts on screen later.
            if (_welcoming && _world.TakeFormResult() is not null)
            {
                _welcoming = false;
                _world.HideForm();
            }

            // Not while a zone is being read. Until it finishes the window is
            // still holding the position it had in the zone being left, and
            // telling the server that is how one zone change becomes several -
            // a position from the old zone can be standing in the line that
            // sent us here.
            if (!_world.IsLoading &&
                _world.Position() is (float x, float vertical, float depth, sbyte direction))
            {
                _session.PlaceAt(x, vertical, depth, direction);
            }

            while (_world.TakeChat() is { Length: > 0 } typed)
            {
                Typed(typed);
            }

            // Somebody clicked on somebody. The renderer decides who was
            // pointed at and hands back their id; this side is the only half
            // with a socket to ask them a question.
            //
            // The click was being collected and dropped: character select read
            // it, the world never did, so clicking an NPC did nothing at all.
            if (_world.TakeTalk() is uint clicked && clicked != 0)
            {
                Talk(clicked);
            }

            // Space: a jump on your feet, a wave lying down. A corpse has no
            // other way to be noticed.
            if (_world.TakeJump())
            {
                Wait(_session.JumpAsync());
            }

            // Whichever button a dead character pressed. The renderer drew the
            // choice; only this half has a socket to say it down.
            switch (_world.TakeDeathChoice())
            {
                case NativeDeathChoice.HomePoint:
                    Wait(_session.ReturnToHomePointAsync());
                    break;

                case NativeDeathChoice.AcceptRaise:
                    Wait(_session.AcceptRaiseAsync());
                    break;
            }

            // Volume and the minimap are changed by keys in the world window,
            // so this side only hears about it afterwards - and writes it out,
            // so the next session starts where this one left off.
            if (_world.TakeSettings() is { } changed)
            {
                MogHouseSettings settings = MogHouseSettings.Current;
                settings.MusicVolume = changed.MusicVolume;
                settings.SoundVolume = changed.SoundVolume;
                settings.RadarTurnsWithPlayer = changed.RadarTurns;
                settings.Save();
            }

            // Somewhere to send a bug from inside the world. The window knows a
            // corner was clicked; opening a browser is this side's job, and
            // works the same on three operating systems.
            switch (_world.TakeLink())
            {
                case NativeLink.Discord:
                    Links.Open(Links.Discord);
                    break;

                case NativeLink.Issues:
                    Links.Open(Links.Issues);
                    break;
            }

            // Pushed rather than raised on change: hit points move constantly,
            // and the panel wants the current number rather than a notification
            // that it moved.
            if (_session.Health is { } vitals)
            {
                _world.ShowVitals(vitals);
            }

            _world.Publish(_tracker);
            Thread.Sleep(50);
        }
    }

    /// <summary>
    /// What a typed line means.
    ///
    /// The client's own commands are answered rather than said: typing /logout
    /// used to be broadcast to the zone as the word "/logout" and do nothing
    /// else, because only the console client ever ran a line past the parser.
    /// </summary>
    private void Typed(string text)
    {
        FfxiClientCommand command = FfxiClientCommands.Parse(text);
        switch (command.Kind)
        {
            case FfxiClientCommandKind.Logout:
                Wait(_session.LogoutAsync(FfxiLogoutKind.Logout));
                _leaving = true;
                return;

            case FfxiClientCommandKind.Shutdown:
                Wait(_session.LogoutAsync(FfxiLogoutKind.Shutdown));
                _leaving = true;
                return;

            case FfxiClientCommandKind.Bug:
                ReportBug(command.Rest);
                return;

            case FfxiClientCommandKind.HomePoint:
                Wait(_session.ReturnToHomePointAsync());
                return;

            // Every channel the real client answers to, and the short forms
            // nobody types the long version of: /s /sh /y /p /l /ls /l2 /u /em.
            case FfxiClientCommandKind.Chat:
                _world.Say(_who, command.Rest);
                Wait(_session.SayAsync(command.Rest, command.Channel));
                return;

            case FfxiClientCommandKind.Tell:
                Wait(_session.TellAsync(command.Recipient, command.Rest));
                // A tell is not echoed back to whoever sent it, so without this
                // the only evidence it went anywhere is the reply.
                _world.Say(">> " + command.Recipient, command.Rest);
                return;

            case FfxiClientCommandKind.Incomplete:
                _world.Say(null, $"/{command.Name} needs more than that.");
                return;

            case FfxiClientCommandKind.Unsupported:
                _world.Say(null, $"/{command.Name} is not something this client does yet.");
                return;
        }

        // Anything starting with '!' is a GM command, which the server routes
        // off the ordinary say path and needs no handling here.
        //
        // Echoed locally either way: the server does not send our own say back
        // to us, so without this talking leaves no trace on screen.
        _world.Say(_who, text);
        Wait(_session.SayAsync(text));
    }

    /// <summary>
    /// Asks whoever was clicked what they have to say.
    ///
    /// The renderer knows only a UniqueNo; the packet wants an ActIndex too,
    /// and the tracker is the one thing holding both. Somebody clicked who we
    /// have never had an update for cannot be addressed, which in practice
    /// means they are out of range or already gone.
    /// </summary>
    private void Talk(uint uniqueNo)
    {
        if (_tracker.Find(uniqueNo) is not { } who)
        {
            _say.WriteLine($"clicked {uniqueNo:X8}, who is not in the tracker");
            return;
        }

        // A tracked entity's name is nullable and plenty of them have none -
        // the server sends NPCs without one and the zone's own name table
        // fills them in later, if at all. Reading it as though it were always
        // there ended the whole session the first time somebody clicked a
        // nameless one, because an exception here unwinds out of the pump and
        // past everything to RunSession's catch.
        string called = string.IsNullOrEmpty(who.Name) ? $"{uniqueNo:X8}" : who.Name;

        try
        {
            _say.WriteLine($"talking to {called}");
            Wait(_session.TalkToAsync(uniqueNo, who.ActIndex));
        }
        catch (Exception failed)
        {
            // Nothing a click can do is worth ending the session over. The
            // world carries on and the log says what happened, which beats a
            // window that stops responding because somebody clicked a rock.
            _say.WriteLine($"could not talk to {called}: {failed}");
        }
    }

    /// <summary>
    /// Writes down what is wrong and where, and sends it on if there is
    /// anywhere to send it.
    ///
    /// <para>
    /// The picture is taken first and waited for briefly: the renderer writes
    /// it on its next frame, and a report of a graphics fault without the
    /// frame it was seen in is most of the way to useless.
    /// </para>
    ///
    /// <para>
    /// Nothing here is allowed to fail loudly. Somebody reporting a bug is
    /// already having a bad time, and a crash while reporting one would be a
    /// poor joke.
    /// </para>
    /// </summary>
    private void ReportBug(string what)
    {
        try
        {
            uint zone = _session.ZoneState?.ZoneNo ?? _openZone;
            string shot = Path.Combine(Path.GetTempPath(),
                                       $"moghouse-bug-{DateTimeOffset.Now:yyyyMMdd-HHmmss}.bmp");
            _world.Capture(shot);

            var now = new FfxiBugReport.Context(
                What: what,
                Who: _who,
                ZoneNo: zone,
                ZoneName: FfxiZoneNames.Label(zone) ?? $"zone {zone}",
                X: _session.PosX,
                Vertical: _session.PosVertical,
                Depth: _session.PosDepth,
                Facing: _session.Facing,
                VanadielHour: _session.VanadielHour,
                GameTime: _session.ZoneState?.GameTime ?? 0,
                Weather: _session.CurrentWeather);

            string? written = FfxiBugReport.Append(now);
            _say.WriteLine($"bug reported: {what}");
            _say.WriteLine($"  written to {written ?? "(nowhere - could not write the file)"}");

            // Give the renderer a few frames to put the picture on disk. It is
            // drawing at sixty a second, so this is a long wait by its
            // standards and an unnoticeable one by anybody else's.
            Thread.Sleep(200);

            // As a PNG. The renderer writes a BMP because that needs no
            // encoder, which is right for a debugging aid and wrong for
            // something to send: a 2560x1440 frame is eleven megabytes
            // uncompressed and Discord will not take it from most accounts.
            // If the conversion fails, send the BMP rather than nothing.
            string? picture = File.Exists(shot) ? (FfxiPng.FromBmp(shot) ?? shot) : null;

            string outcome = FfxiBugReport.SendAsync(now, picture)
                                          .GetAwaiter().GetResult();
            _say.WriteLine($"  {outcome}");
            _world.Say(null, $"Reported: {what} ({outcome}).");
        }
        catch (Exception failed)
        {
            _say.WriteLine($"could not report a bug: {failed}");
            _world.Say(null, "Could not write that report down - it is in the log.");
        }
    }

    private void OnChat(FfxiChatLine line) => _world.Say(line.Sender, line.Text, line.Kind);

    private void OnEntities(IReadOnlyList<FfxiEntityUpdate> entities)
    {
        // The tracker is what the renderer draws from: it holds what an entity
        // looks like, whether the server means it to be seen, and which fields
        // a partial update may change.
        DateTimeOffset now = DateTimeOffset.UtcNow;
        foreach (FfxiEntityUpdate update in entities)
        {
            _tracker.Observe(update, now);
        }
    }

    private void OnDeathChanged(bool dead) => _world.ShowDeath(dead, _session.HasRaiseOffer);

    private void OnRaiseOfferChanged(bool offered) => _world.ShowDeath(_session.IsDead, offered);

    private void OnMovedByServer() =>
        _world.PlaceCharacter(_session.PosX, _session.PosVertical, _session.PosDepth, _session.Facing);

    /// <summary>
    /// The server sends a track number; the file it names has to be found.
    ///
    /// Which sound directory holds it is not fixed - the install spreads them
    /// across sound, sound2 and so on as expansions were added - so this looks
    /// rather than computes, and a track this install does not have goes quiet
    /// rather than failing.
    /// </summary>
    /// <summary>
    /// The weather turning while somebody is standing in it.
    ///
    /// Handed on so the next zone comes up under the right sky. It does not
    /// change the one overhead: a zone's sky is built with the zone, and
    /// swapping it without reloading everything else needs a rebuild path that
    /// does not exist yet.
    /// </summary>
    private void OnWeatherChanged(FfxiWeather weather)
    {
        _say.WriteLine($"the weather turned to {weather}");
        _world.ShowWeather(weather);
    }

    private void PlayMusic(int track)
    {
        string? path = FfxiMusicFile.Resolve(FfxiInstall.Find() ?? "", track);
        if (path is null && track > 0)
        {
            _say.WriteLine($"no file for music track {track}");
        }
        _world.ShowMusic(path);
    }

    /// <summary>
    /// Walking into a zone line, or being sent somewhere.
    ///
    /// The window draws the new zone itself rather than being replaced by one
    /// that does - closing it was what made zoning look like the client
    /// shutting down, because for as long as the new zone took to read there
    /// was nothing on screen at all.
    /// </summary>
    private void OnZoneChanged(uint zone)
    {
        // Target indices are only unique within a zone, so carrying one across
        // would put an old entity's name and kind on whatever now holds that
        // index.
        _tracker.Clear();

        if (zone == _openZone)
        {
            // A teleport inside the zone you are already standing in still
            // arrives as a zone change: !goto and !bring both send you through
            // the zone server even when the destination is where you already
            // are. There is nothing to load, and there is somewhere new to
            // stand - returning without doing anything is what made those two
            // commands look like they did nothing at all.
            _world.PlaceCharacter(_session.PosX, _session.PosVertical, _session.PosDepth, _session.Facing);
            _say.WriteLine($"moved inside zone {zone} to " +
                           $"{_session.PosX:F1} {_session.PosVertical:F1} {_session.PosDepth:F1}");
            return;
        }

        string name = FfxiZoneNames.Label(zone) ?? $"zone {zone}";
        _say.WriteLine($"zoning to {zone} ({name})");

        // Told before the zone is read, because that is when its sky is built.
        _world.ShowWeather(_session.CurrentWeather);

        if (_world.LoadZone((int)zone, name, _session.PosX, _session.PosVertical, _session.PosDepth,
                            _session.Facing))
        {
            _openZone = zone;
            _world.ShowZoneLines(_session.ZoneLines);
            PlayMusic(_session.CurrentTrack);
            return;
        }

        // No DAT for that zone. Saying so beats a window that silently keeps
        // drawing the zone the character has already left.
        _say.WriteLine($"could not read {name}; still showing zone {_openZone}");
        _world.Say(null, $"{name} is not in this installation.");
    }

    /// <summary>
    /// Drives a task to completion on this thread.
    ///
    /// This loop is already on a thread of its own and everything in it is a
    /// step in a sequence, so awaiting would only hand the work to a pool
    /// thread and wait for it anyway.
    /// </summary>
    private static void Wait(Task work) => work.GetAwaiter().GetResult();
}
