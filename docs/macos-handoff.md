# Building MogHouse on macOS

Written on the Windows machine for whoever picks this up on the Mac. Nothing
here has been run on macOS — treat every claim below as "this is what the code
says", not "this is known to work". Where something is genuinely unknown it
says so rather than guessing.

The user's day-to-day machine is the Mac, so this is not a port of a Windows
project to a second platform. It is the other way round: Windows is the
machine that happened to be free tonight.

## Version, before anything else

The client requires a Final Fantasy XI installation on the **August 2026**
patch, and speaks to servers of that same version. It is not backwards
compatible, and the failures are quiet rather than loud: file ids move between
versions, so the wrong model loads; packet layouts shift, so fields are read
from the wrong offsets. A Mac install obtained by copying an older Windows
installation is the likely trap here.

## What is already cross-platform

The client is .NET 10 and Avalonia, and the renderer is Dawn/WebGPU and SDL3.
Both of those stacks run on macOS, and the code does not assume otherwise:

- `NativeViewer.Resolve` already looks for `libmoghouse_interop.dylib` on
  macOS, and for `.so` on Linux (`src/MogHouse.Core/Interop/NativeViewer.cs`).
- Every data path falls back to a directory beside the executable, so nothing
  needs environment variables to run — see "Paths" below.
- **Dawn's Metal backend has been proven on an M4** for a related project.
  What has *not* been proven is this renderer on it.

## What will need doing

### 1. Build the renderer

    cmake -S . -B build-renderer -G Ninja
    cmake --build build-renderer

Two things to expect trouble from:

- **Dawn.** `vendor/dawn-install` on Windows is a prebuilt Dawn with
  `webgpu_dawn.lib` statically linked into the shim. The Mac needs its own
  Dawn build; there is no universal binary here. Building Dawn is the long
  pole — budget for it taking most of the time.
- **SDL3.** `vendor/SDL3-3.4.14` is the Windows binary distribution. Use
  Homebrew's SDL3 or build it; the version matters less than that it is 3.x,
  since the code uses the SDL3 audio callback API (`SDL_OpenAudioDeviceStream`
  with a callback), which SDL2 does not have.

The renderer builds several small tools besides the client shim. If Dawn is
fighting you, `ffxi-datdump`, `ffxi-collisiondump` and `ffxi-chardump` do not
need a GPU and are a good way to prove the DAT reading works before the
graphics do.

### 2. Metal-specific unknowns

Honestly flagged rather than predicted:

- **The surface.** SDL3 gives a `CAMetalLayer`; Dawn wants one too. This is
  the most likely place to spend an afternoon.
- **WGSL.** The shaders should be portable, but Metal's compiler is stricter
  than D3D12's about some things. There are four shader modules:
  `zone_shader.h`, `radar_shader.h`, `zoneline_shader.h` and the nameplate/HUD
  one. If the window comes up black, that is the first place to look — a WGSL
  error takes down the whole module, not one line. Run the standalone
  renderer for one frame and count the errors:

      MOGHOUSE_ZONE_NAME="Bastok Markets" ./build-renderer/moghouse-renderer <zone DAT> --frames 1

  A clean run prints an adapter line, a zone line and no `webgpu error`.
- **Depth format and MSAA** are chosen in `viewer.cpp`; if Metal rejects one,
  it will say so through the error callback, which now reaches the log.

### 3. Case-sensitivity

Windows does not care about filename case and macOS's default volume does not
either, but a case-sensitive volume would. The DAT paths are built from
numbers so they are safe; the asset names (`font.bin`, `subrooms.txt`,
`<Zone_Name>.water`) are matched exactly as written.

### 4. The app bundle

There is no `.app` bundle yet. `tools/package-windows.ps1` is the Windows
equivalent and is worth reading for what has to travel with the client. The
shape it settled on, which a bundle should match:

    MogHouse XI.exe        one file: the .NET runtime and every managed
                           assembly are published inside it
    README.txt
    data/                  everything else, hidden by the client on first run
      libmoghouse_interop.dylib    the renderer
      libSDL3.dylib                unless statically linked
      assets/font.*, assets/subrooms.txt
      assets/water/*.water         ~50MB
      keys/*.bin                   required; nothing decrypts without them
      res/compress.dat, decompress.dat
                                   required; no server connection without them
      zones/                       optional; zone lines

The two native libraries cannot go inside the single file: the renderer looks
for its assets beside whichever directory the library was loaded from, so they
and the assets have to be real files in the same place. On macOS a bundle
already has the right shape for this — `Contents/MacOS` for the executable and
`Contents/Resources` for the rest — so the hidden-folder trick is a Windows
answer to a problem a bundle does not have.

The game's own DATs are never shipped. `FfxiInstall.Find()` locates an
existing installation — **it is Windows-registry-and-Program-Files shaped, and
will need a macOS branch.** On a Mac the game usually lives under a Wine or
CrossOver prefix, so the likely answer is to look in the obvious prefix
locations and otherwise ask, which the installer page already supports.

## Paths

Everything falls back to a directory beside the executable, so a copied folder
runs with nothing set. Environment variables override, and are what
`tools/app.sh` uses for a development checkout:

| Variable | Falls back to | Needed? |
|---|---|---|
| `MOGHOUSE_FFXI_RES` | `<exe>/res`, then `<exe>/data/res` | **Yes** — no server connection without it |
| `MOGHOUSE_FFXI_KEYTABLE` | `<exe>/keys/`, then `<exe>/data/keys/` | **Yes** — no zone decrypts |
| `MOGHOUSE_FFXI_KEYTABLE2` | as above | **Yes** |
| `MOGHOUSE_FONT` | `assets/` beside the loaded library | Yes — no HUD or nameplates |
| `MOGHOUSE_SUBROOMS` | `assets/subrooms.txt` beside the library | No — buildings become empty shells |
| `MOGHOUSE_FFXI_ZONEDATA` | `<exe>/zones`, then `<exe>/data/zones` | No — no zone lines, use `!zone` |
| `MOGHOUSE_FFXI_NAVMESHES` | `<exe>/navmeshes`, then `<exe>/data/navmeshes` | No — only feeds the launcher's flat map |
| `MOGHOUSE_LOG` | none | Strongly recommended, see below |
| `MOGHOUSE_BODY_DISTANCE` | unset = no limit | No — how far away bodies are drawn |

## Read the logs first

This is the single most useful thing to know. The client is a windowed app
with no console, and for a long time everything it said went nowhere, which
meant bugs could only be reasoned about — twice at length, and wrong both
times.

- `moghouse.log` — the managed client: logins, zone changes, session status.
- `moghouse.log.renderer` — the renderer: zones read, water loaded, models
  built, textures uploaded, WebGPU errors.

Two files rather than one because the .NET side holds its log open in a way
that shares for reading only, and two writers on one file tread on each other.
`redirectOutputToLog` in `native/moghouse_interop/src/moghouse_interop.cpp`
probes before it redirects, because `freopen` closes the stream *before* it
tries to open the new one — a failed redirect there left stdout shut and every
later `printf` writing to a dead handle, which stopped the world window from
opening at all.

## Water

`renderer/assets/water/*.water` is generated, not committed: about 50MB across
185 zones, derived from LandSandBoat's collision meshes. Regenerate with

    python tools/makewater.py

which needs the server's `ximeshes` directory (`MOGHOUSE_FFXI_XIMESHES`, or
edit `DEFAULT_ROOT` in `tools/ximesh.py`). Takes about a minute. Without these
files every canal and sea is dry.

They do ship in the 0.1.2 Windows release. They are derived from game data, so
whether that stays true is the user's call rather than an assumption to carry
forward.

## Where things stand

`docs/roadmap.md` is the honest list of what is not done, kept because half of
it was found by playing rather than by reading code. Worth reading before
picking a task.

`docs/wiki/Coordinates-And-Rotation.md` is worth reading before touching
anything positional. The short version: the world is `(x, -y, -z)` — a
rotation, not a reflection — and headings are `pi/2 - r`, not `pi - r`. Both
mistakes have been made and both look plausible on screen.

The lesson this project keeps relearning, and which cost the most time:
**two halves of one client agreeing tells you they were written by the same
person, not that either is right.** The minimap was mirrored for weeks and
survived every check, because the map bake, the sampling and the dots all
shared the mirror and agreed with each other perfectly. Only the 3D camera
disagreed. Validate against something you did not produce — the server's
numbers, a retail client, the game's own files.
