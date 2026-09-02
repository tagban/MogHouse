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

---

# What happened on the Mac, 2026-09-01

Written by the macOS session. The client builds, runs, renders a zone and
packages into a signed `.app`. Two real bugs were fixed on the way, neither of
them macOS-specific in cause.

## Dawn was not the long pole

The brief says to budget most of the time for Dawn. It cost none. The "related
project where Dawn's Metal backend was proven on an M4" is on this same Mac,
and `build-dawn.sh` here has byte-identical flags to that project's, so the
install prefix dropped straight in:

    vendor/dawn-install -> /Volumes/AppStorage/PortJeuno-build/dawn-install

21MB, Dawn `053ad3188`. If it ever does need rebuilding, that took 3m41s at
745% CPU - far short of "most of your time".

**SDL3 was a non-issue too.** `find_package(SDL3)` found Homebrew's 3.4.14 with
no `CMAKE_PREFIX_PATH` - the same version as the vendored Windows package.

## Two commands in this document do not work

**`cmake -S . -B build-renderer` cannot work: there is no root
`CMakeLists.txt`.** The entry point is `renderer/`, which is what
`build-renderer.sh` correctly uses.

**`--frames 1` is not a flag.** `renderer/main.cpp` reads `argv[1]` as the zone
path and takes everything else from the environment; unknown arguments are
ignored silently, so the renderer opens its interactive window and sits there
until Escape. The one-frame check the brief is reaching for is:

    MOGHOUSE_SCREENSHOT=/tmp/shot.png MOGHOUSE_SCREENSHOT_AFTER=8 \
      ./build-renderer/moghouse-renderer <zone DAT>

which renders, writes the PNG and exits 0.

## Two bugs, both latent on Windows

**`viewer.cpp:3074` did not compile.**

    const bool pinnedClip = options.animation ? options.animation->c_str() : nullptr != nullptr;

`options.animation` is `std::optional<std::string>`. This is a refactor
artefact: `options.animation != nullptr` had the optional-to-`const char*`
idiom substituted into it, and because `!=` binds tighter than `?:` the else
branch collapsed to `nullptr != nullptr`, a `bool`, against a `const char*`
then branch. MSVC accepts the mismatch through pointer-to-bool and computes the
right answer by accident; Clang rejects it. Replaced with
`options.animation.has_value()`, which is what MSVC was computing and what the
comment above it describes.

**The renderer segfaulted on every exit.** `SIGSEGV` in `pthread_mutex_lock`,
under `SDL_DestroyAudioStream`, under `mh::Music::~Music()`:

    music is a local declared at viewer.cpp:3017
    SDL_Quit() is called at the end of runViewer
    ~Music() therefore runs *after* SDL_Quit

Destroying an SDL audio stream once the audio subsystem is gone locks a mutex
that has already been freed. That is undefined on every platform - Windows just
survives it. Fixed by adding `Music::shutdown()`, idempotent and called both by
the destructor and explicitly before `SDL_Quit`. Verified: repeated headless
runs now exit 0 where they exited 139.

This one is worth taking back to Windows. It is not a macOS bug; it is a
teardown-order bug that macOS is simply strict enough to catch.

## Zone rendering works on Metal

    adapter: Apple M4 (Metal)
    window: 1280x720 points, 2560x1440 pixels
    161 models (0 unreadable), 4918 placements drawn, 0 with no model
    collision: 427794 triangles, 227791 walls
    zone f_sa: 41178 triangles

**Zero `webgpu error`.** All four WGSL modules compiled on Metal with no
complaint, so the brief's "if the window is black, suspect WGSL first" did not
come up. Textures, the baked map, the glyph atlas, the radar and the lighting
clock all initialise. East and West Sarutabaruta both render and were confirmed
by eye.

## Packaging: `tools/package-macos.sh`

Produces a signed `MogHouse XI.app` and a zip. Four things it has to do that
the Windows script does not, each of which is a trap:

**Resources cannot live in `Contents/MacOS`.** `codesign` treats everything
there as code and refuses to seal a bundle containing a `font.png`. But the
client looks for `AppContext.BaseDirectory/data` in five separate places. The
answer that needs no C# change: the real files go in `Contents/Resources/data`,
with a symlink at `Contents/MacOS/data` pointing at them.

**The hardened runtime kills CoreCLR.** Signing with `--options runtime` - which
notarization requires - produces a bundle that verifies and then dies at launch
with `Failed to create CoreCLR, HRESULT: 0x80070008`. The JIT needs memory that
is writable and executable, which the hardened runtime forbids. Three
entitlements fix it: `allow-jit`, `allow-unsigned-executable-memory`, and
`disable-library-validation` (the bundle loads its own renderer and a copy of
SDL3 that Apple did not sign).

**SDL3 is linked by absolute path.** `otool -L` shows
`/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib`, so a copy of the bundle would not
start on a Mac without Homebrew. The script copies the library in and rewrites
the reference with `install_name_tool` to `@loader_path`.

**`ditto`, not `zip`.** A plain zip drops the symlinks and extended attributes
the signature depends on.

Verified: the bundle launches with `DOTNET_ROOT` and every `MOGHOUSE_*`
variable unset, so the self-contained runtime and the path fallbacks both work.
`Info.plist` gives it a real identity - launched with `open`, the menu bar says
"MogHouse XI" rather than "Avalonia Application". (Running
`Contents/MacOS/MogHouse XI` directly bypasses the bundle and still shows the
old name; that is the launch method, not a bug.)

**Notarized and stapled.** Apple accepted submission
`99c43e32-4405-43a6-9510-79bd72c7e6d4`; the ticket is stapled into the bundle
and `spctl` reports `accepted, source=Notarized Developer ID`. Verified the way
a downloader sees it - the zip extracted to a clean directory, where Gatekeeper
accepts it, `stapler validate` passes (so the ticket travels inside the archive
and works with no network) and `codesign --verify --deep --strict` verifies.

Two ordering traps, both easy to get wrong:

**Staple before zipping.** The ticket attaches to the `.app`, so an archive made
before stapling does not contain it and the download is unnotarized however the
local copy reports. Re-run the `ditto` step afterwards.

**Notarizing needs a human.** It uploads the build to Apple and authenticates
with an Apple ID and an app-specific password:

    xcrun notarytool submit "<absolute path>/MogHouse-XI-Alpha-0.1.2-macos-arm64.zip" \
      --apple-id <id> --team-id D36X678376 --password <app-specific> --wait
    xcrun stapler staple "dist/MogHouse XI.app"

Use an absolute path: notarytool resolves relative to the working directory, and
its "file doesn't exist" error reads like an upload failure. Better still, run
`xcrun notarytool store-credentials "MogHouse" --apple-id <id> --team-id
D36X678376` once and pass `--keychain-profile "MogHouse"` after that, which
keeps the password out of shell history.

## Install detection was already better than this document says

`FfxiInstall.Likely()` already carried macOS, Wine and CrossOver candidates, so
"it is Windows-registry-and-Program-Files shaped and will need a macOS branch"
is out of date. What was actually missing: it hardcoded a single CrossOver
bottle named `FFXI`, and bottles are named by whoever made them. Now enumerated
properly, plus Whisky - the wrapper most Mac players use now - and `WINEPREFIX`,
and both `Program Files` and `Program Files (x86)` inside each prefix.

Note that a game folder on an external volume, which is where this machine's
lives, is not something detection can reasonably guess. The picker handles it,
and the picker is a native macOS file dialog that works.

## What is not done

- **`res/`** - `compress.dat` and `decompress.dat`. Not in a retail install;
  they come from LandSandBoat, which is not checked out on this machine. The
  client starts and renders without them and says so plainly on the first line
  of its log, but it cannot connect to any server. `--res` is a parameter of
  the packaging script for whoever has them.
- **Water** - `renderer/assets/water/*.water` is generated from the server's
  `ximeshes`, which needs LandSandBoat too. Canals and seas are dry.
- **Notarization** - above.
- **Architecture** - arm64 only. Dawn, SDL3 and the publish are all arm64;
  nothing here is universal, and an Intel Mac is untested.
