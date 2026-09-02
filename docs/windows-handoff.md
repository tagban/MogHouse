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
| `src/MogHouse.Core/Screens/` | the in-engine screens that replaced the Avalonia ones |
| `renderer/monorail.h` | the Sel Phiner monorail, driven entirely client side |
| the wiki's Audio-Formats page | `.bgw` and `.spw`, and the split sample rate |

## Still open

### Avalonia cannot be deleted yet, and it is worth being precise about why

The screens moved into the renderer and the client draws its own: install
picker, sign-in, account creation, character select and character creation all
run in the one window now, behind `--screens`. That is the *pre-game* path, and
it is done.

The *in-game* layer is not. `MogHouse.App/ViewModels/GameViewModel.cs` is 831
lines and almost none of it is user interface - it is the wiring between the
session and the renderer, and it has no replacement. `ClientFlow.EnterWorld`'s
loop is twelve lines: report where the character walked to, publish the
entities, sleep.

Everything in this table lives only in `GameViewModel` and would go with it:

| | |
|---|---|
| chat | `Say`, the typed line, what the server sends back |
| jumping | `TakeJump` to `JumpAsync` |
| death | `IsDead`, `HasRaiseOffer`, `AcceptRaiseAsync`, `ReturnToHomePointAsync`, `ShowDeath` |
| zoning | `ZoneLines`, `ShowZoneLines`, `LoadZone` between zones |
| vitals | `Health` to the HP/MP/TP bar |
| music | `CurrentTrack` and `CurrentPath` per zone |
| settings | `ShowSettings` and what comes back |
| navigation | `NavMesh` |

So the job is to port that wiring into `ClientFlow`, which is mostly moving
game logic out of a view model it never belonged in. Deleting Avalonia is
trivial afterwards and destructive before.

### Parked for whoever picks this up

**A seated pose while riding the monorail.** The bodies carry 137 animation
clips and `MOGHOUSE_CLIPS` lists them. Six are sitting - `si00` and `si01`,
`si10` and `si11`, `si20` and `si21` - which is three poses, each split into a
lower and an upper body track the way `idl0` and `idl1` are. The renderer plays
clips by name and `MOGHOUSE_ANIMATION` picks one, so this is small: render the
three, choose one, play it while `riding()`.

Seats as objects is the harder half and probably not worth it. Zone geometry is
baked from the DAT's placement list at load, so nothing new can be added -
only placements that already exist can be moved, which is the trick the train
itself uses.

**Sound effects.** The format is worked out and written up on the wiki as
[Audio-Formats]; `tools/spwdecode.py` turns a `.spw` into a WAV and handles
8,488 of the 9,459 in a retail install. What is missing is a place to play them:
`Music` is one stream for one `.bgw` with no mixing and no positional audio. An
effects channel wants a second SDL audio stream, several voices, and volume
from the distance to whatever is making the noise - the monorail was the case
that raised it. Finding a usable rumble among 9,459 files is its own afternoon.

**Bastok Mines blows out to white around noon and goes black at night.** A
different fault from the one already fixed: that was zones shipping no lighting
at all, and this zone has plenty. It is that an underground zone is lit by the
outdoor day and night cycle, and only the thirteen rooms that ship their own
lighting escape it. `MOGHOUSE_TIME=1200` reproduces it immediately.

### The old thread bug, for the record

The Avalonia client black-screened on zone load because `LiveRadar` defaulted to
its own thread and AppKit will not make a window anywhere but the main one. That
is fixed - `ownThread: false` plus a non-async `Main` - and the restructure it
forced is what the screens work was built on.
