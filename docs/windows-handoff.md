# Handing back to Windows

Written on the Mac, 2026-09-02, after a session that got MogHouse running end
to end on macOS. Two changes touch Windows and want your judgement; one bug is
almost certainly reproducible there.

Everything below is on `master`.

## What now works on macOS

Against a LandSandBoat built on the Mac for the purpose:

- login, character select, character creation
- zone-in, with the world drawn - Bastok Mines, 223 models, 13 building
  interiors, 103791 triangles of collision, 1477 draws
- movement reaching the server, persisted while walking and across a relogin
- the player's own nameplate, and other players' nameplates
- two clients at once, each seeing the other

None of it needed changes to the protocol, the DAT reading, the shaders, Dawn,
or `surface_metal.mm`. All of that was already right.

## 1. LiveRadar can run the render loop on the caller's thread

`LiveRadar.Open` takes a new `ownThread` parameter, defaulting to `true` - the
old behaviour - so nothing existing changes.

macOS refuses to create an `NSWindow` anywhere but the main thread, and SDL
reports that as `No available video device`, which reads like a driver fault
and is not one. `LiveRadar` starts the loop on a thread of its own, and that
loop creates the window, so the Avalonia client black-screens after a
successful login. Windows has no such rule, which is why it went unnoticed.

`ownThread: false` leaves the loop unstarted for the caller to run with
`LiveRadar.Run()`. `tools/loginzone` is a small working reference for the
arrangement: log in first, on the main thread, while there is no window - then
hand that same thread to the render loop while the session runs on background
threads.

**One trap it is easy to fall into.** `Main` must not be `async`. After the
first `await`, a console app resumes on a thread-pool thread - there is no
synchronisation context to come back to - and the window creation then fails
exactly as it does in the client today. Calling `Run()` from `Main` is not
sufficient on its own.

The client restructure this points at is described in `docs/ui-in-renderer.md`,
including why removing Avalonia dissolves the problem rather than working
around it. That changes control flow on both platforms, which is why it wants
someone on Windows to test it there.

## 2. The nameplate gate - likely a Windows bug too

The whole nameplate pass was gated on there being other entities:

    if (platePipeline && plateBindGroup && !radarEntities.empty())

The player's own plate is laid out inside that block, before the entity loop,
and never came from the entity list. So a character standing alone in a zone
was always anonymous - exactly the case where a name over your own head is the
only one on screen. The inner `named > 0` check already skips the draw when
there is nothing to say.

Nothing about that is macOS-specific. **Worth confirming on Windows**: stand
somewhere with no NPCs or players in range and see whether your own name is
drawn. It probably was not.

## 3. Diagnostics added to FfxiZoneClient

`ReceiveAsync` now distinguishes its two failure modes, which used to be
indistinguishable and mean opposite things:

    zone: nothing arrived within 2s
    zone: 484 bytes from 127.0.0.1:54230, checksum ok, declared bits 3486, no plaintext

The first says look at the network; the second says look at the key or the
codec. Both used to surface as "Zone server did not answer, or its reply could
not be decoded".

Be aware the first line is **not** necessarily a fault: with nothing happening
nearby the server has nothing to send. It cost me hours to work that out - see
`docs/networking-handoff.md`, which is now a post-mortem of a bug that did not
exist rather than a description of one that does.

## Testing against the Mac's server

The Mac runs a LandSandBoat at **10.0.0.11**. The servers bind `0.0.0.0`, so a
Windows client can point at it directly. If it will not connect, macOS's
firewall is the first thing to check - `xi_connect`, `xi_map` and `xi_world`
each need to be allowed to accept incoming connections.

Two accounts, both with password `mhtestpw123`:

| Account | Character |
|---|---|
| `mhtest` | Testy |
| `mhtest2` | Duo |

A Windows client walking around against this server would be a genuinely useful
test: it separates anything left in the Mac client from the protocol, and the
Mac side can watch the server's own view while it happens - database rows, zone
counters, map logs.

`docs/local-test-server.md` covers how that server was built, which was not
obvious: Apple's clang has no `std::jthread`, Homebrew's current LLVM is too
new, LLVM 20 is the one that works, and its libc++ has to be linked rather than
only its headers used.

## Where to look

| | |
|---|---|
| `docs/ui-in-renderer.md` | the client restructure, and why Avalonia goes |
| `docs/macos-handoff.md` | what happened on the Mac, from the beginning |
| `docs/networking-handoff.md` | a false lead, kept so nobody repeats it |
| `docs/local-test-server.md` | building and running LandSandBoat on macOS |
| `tools/loginzone` | working reference for the main-thread arrangement |

## Still open

The Avalonia client itself. It builds and runs on macOS, shows the installer
and login, and connects - and then black-screens on zone load, because
`LiveRadar` still defaults to its own thread. The fix is not a rebuild and not
macOS-specific; it is which thread creates the window, and the same restructure
suits all three platforms.
