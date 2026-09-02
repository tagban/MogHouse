# Handing back to Windows

Written on the Mac, updated 2026-09-02 after a second session that moved the
client's screens into the renderer. Everything below is on `master`, which is
the branch - there is no `main`.

## Read this first: none of it has run on Windows

Every change described here was written and tested on macOS only. Nothing about
it is deliberately platform-specific, and the parts that were - the thread the
window is created on, chiefly - were fixed in a way meant to suit all three.
But "meant to" is not "checked", and the first useful thing a Windows session
can do is find out which half of that is true.

The likeliest places for it to bite:

- **The renderer builds with Homebrew's clang here**, because Apple's has no
  `std::jthread`. MSVC has it. `tools/package-windows.ps1` is the Windows build
  and has not been run since any of this landed.
- **`NativeEnvironment.Set`** exists because on Unix `Environment.SetEnvironment
  Variable` does not reach native `getenv`. On Windows it always did, so that
  path is a no-op there and should stay harmless.
- **Install detection** goes through the registry on Windows and through a list
  of likely folders and Wine prefixes elsewhere. The registry path is untouched.
- **The interface is now scaled** by the ratio between the window's points and
  its pixels. On a 1080p display that ratio is 1 and nothing should change; on a
  4K Windows display with scaling it is the thing to look at first.

## What the client does now

One window for everything. The sign-in, account creation, character select and
character creation are drawn by the renderer as forms, over a live zone rather
than in front of a black screen - and character select is the characters
themselves standing in Sel Phiner, faded until the cursor is over them, with a
blank Mithra at the end for making a new one. It runs behind `--screens`;
without it the old Avalonia launcher still starts.

Then the world: walking, chat and the commands a typed line can be, jumping,
death and raise and homepoint, zoning between zones, vitals, music, settings.

Verified against a LandSandBoat running on the Mac: sign in, make an account,
click a character, zone in, walk about, two clients seeing each other.

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

## Running it on Windows

```powershell
dotnet run --project src/MogHouse.App/MogHouse.App.csproj --no-launch-profile -- --screens
```

Without `--screens` you get the old Avalonia launcher, which is still the
default.

The renderer needs the two key tables in `keys/` beside the repository -
`mzb_key_table.bin` and `mmb_key_table2.bin`. They are built by
`tools/keytables.py` rather than shipped, and without the first one a zone will
not decode: the client now says so on a screen before asking for a password
rather than showing a black world.

Worth having while you work:

| | |
|---|---|
| `MOGHOUSE_TIME=1700` | holds the hour still, so two runs are comparable |
| `MOGHOUSE_UI_SCALE` | interface size on top of the display's own correction |
| `MOGHOUSE_SCENE_ZONE` | the zone behind the sign-in; 0 is Sel Phiner, -1 is none |
| `MOGHOUSE_LOG` | where the client writes; the renderer adds `.renderer` |

The standalone renderer takes a zone DAT and needs no server at all, which is
how most of the graphics work was done:

```powershell
.\build-renderer\moghouse-renderer.exe "$env:MOGHOUSE_FFXI_INSTALL\ROM\0\28.DAT"
```

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
