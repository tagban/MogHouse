# The session that was never broken

Written on the Mac, 2026-09-02. This file previously described a networking bug
in some detail. There was no bug. It is kept, rewritten, because the false lead
took hours and the way it fooled me is worth not repeating.

## What actually works

Verified against a local LandSandBoat, on macOS, with the renderer on the main
thread:

- login, character select and character creation
- zone-in, and the world drawn: Bastok Mines, 223 models, 13 building
  interiors, 103791 triangles of collision, 1477 draws
- **movement reaching the server** - the character's position is persisted while
  walking, and survives a logout and relogin
- **your own nameplate**, once the gate described below was removed
- **two clients at once**, each seeing the other's character and nameplate, with
  the map server reporting `IncreaseZoneCounter <2>`

So the protocol, the session, the entity feed and the renderer are all fine on
macOS. The only genuine bug in this whole investigation was the nameplate gate.

## The false lead, and why it was convincing

The symptom looked serious: the client sent position every 400ms, received
nothing at all, and the map server dropped the session about 68 seconds in -
which matches `MAX_TIME_LASTUPDATE = 60` almost exactly.

That is what the evidence looked like. What was actually happening:

**The test client was exiting.** Those runs had `MOGHOUSE_SCREENSHOT` set, so
the renderer wrote its frame and closed. `window closed, rc=0` was in the output
the whole time, and I read past it. The client quit; the server reaped the
session a minute later, exactly as designed. There was never a rejected packet.

**A diagnostic that only logged failures.** `zone: nothing arrived within 0.4s`
looks like a fault and is not one: with nothing happening nearby, the server has
nothing to send. Successful receives logged nothing, so ordinary silence was
indistinguishable from breakage - and I had added that line myself, to
distinguish two failure modes, without considering that neither might apply.

**Two plausible theories built on top.** First that packets were being rejected
before `MapNetworking::parse`, since `last_update` was never refreshed. Then, on
the strength of the `0x00A` handler's comment that "Key is already assumed to be
incremented correctly", an elaborate key-rotation deadlock: the client's only
cue to advance its key is a failed checksum in a reply, and no reply comes, so
neither side can move. It fit every measurement. It was entirely wrong.

The lesson is the one this project already knows, in a new costume: **the
evidence and the theory agreed with each other because I built the theory from
the evidence.** What broke it was someone picking up the keyboard and walking
the character around - a fact from outside the loop.

## The one real bug

The whole nameplate pass was gated on `!radarEntities.empty()`:

    if (platePipeline && plateBindGroup && !radarEntities.empty())

Our own plate is laid out inside that block, before the entity loop, and never
came from the entity list. So a character alone in a zone was always anonymous -
exactly when a name over your own head is the only one on screen. The inner
`named > 0` check already skips the draw when there is nothing to say.

Not macOS-specific. It would show on Windows for anyone standing alone.

## Feeding entities, which the harness needed

`tools/loginzone` originally fed only position back to the session, so no other
player was ever drawn. The pattern the real client uses, and which the harness
now copies:

    var tracker = new FfxiEntityTracker { SelfUniqueNo = state.UniqueNo };
    session.EntitiesChanged += updates =>
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var update in updates) tracker.Observe(update, now);
    };

    // on the same tick that reports position
    world.Publish(tracker);

With that, two instances see each other.

## Running two clients

Two accounts exist on the local server, both with password `mhtestpw123`:
`mhtest` (character Testy) and `mhtest2` (Duo). Start each with its own
`MOGHOUSE_LOG` so the logs do not collide, and note that a killed client holds
its session for about 60 seconds before the server will accept it again -
`MAX_TIME_LASTUPDATE`, the same timer that misled me above.

Both windows open at the same default position, so the second lands exactly on
top of the first. Move one before deciding nothing happened.

## What is still open

Nothing in the networking. The remaining macOS work is the one already known:
`LiveRadar` still starts the render loop on its own thread by default, so the
Avalonia client still black-screens. `Open(..., ownThread: false)` and
`docs/ui-in-renderer.md` are the path out of that, and `tools/loginzone` is the
working reference for the shape it should take.
