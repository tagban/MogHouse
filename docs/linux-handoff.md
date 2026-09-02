# Building MogHouse on Linux

Written on the Mac for whoever picks this up on Linux, in the same spirit as
`docs/macos-handoff.md` was written on Windows for the Mac. Nothing here has
been run on Linux — treat every claim as "this is what the code says", not
"this is known to work". Where something is genuinely unknown it says so.

Read `docs/macos-handoff.md` too, particularly "What happened on the Mac". Two
of the bugs found there were not macOS bugs at all, and one of them is very
likely waiting on Linux as well.

## Where this stands

Done already, and none of it needs redoing:

- **The renderer is portable.** `renderer/surface_linux.cpp` already exists
  alongside the Windows and Metal ones, and `renderer/CMakeLists.txt` selects
  it. Dawn picks Vulkan on Linux the same way it picks Metal on macOS.
- **The writable-state problem is fixed.** The client used to write
  `moghouse-settings.json`, `ffxi-server-profiles.json`, `ffxi-install.json`
  and its logs beside the executable, which fails inside a Flatpak because
  `/app` is read-only. `FfxiServerProfileStore.DefaultConfigDirectory()` now
  probes whether that directory is writable and falls back to
  `LocalApplicationData` — `$XDG_DATA_HOME`, which a Flatpak redirects into its
  own sandbox. Writing beside the executable still wins wherever it works, so
  the portable-zip behaviour is unchanged. Verified on macOS by making the
  directory read-only and watching it fall back correctly.
- **Logs now default.** They previously wrote nowhere at all unless
  `MOGHOUSE_LOG` was set, which meant a released build was silent — bad, given
  the README tells testers to attach both logs. They now default into the same
  writable directory, and the renderer still derives its own file by appending
  `.renderer`.
- **`flatpak/com.tagban.MogHouse.yml`** exists, with a `.desktop` and a
  metainfo file. UNTESTED.
- **`tools/package-linux.sh`** exists, producing the `dist/linux-x64` tree the
  manifest packages. UNTESTED. It is short; read it rather than trusting it.

Not done: anything actually compiled or run on Linux.

## Setting up WSL2

WSL2 is the right first target on this machine: it is x86_64, which is what
Linux players actually run, and unlike an ordinary VM it has real GPU
passthrough — NVIDIA's WSL driver exposes working Vulkan, so the 4060 is
visible to Dawn. That matters, because "does the renderer come up on Vulkan" is
the one question a VM usually cannot answer.

    wsl --install Ubuntu-24.04

24.04 LTS rather than 26.04: the newer one is too fresh for the .NET and
Flatpak packaging to be predictable, and this is not the place to be debugging
the distro.

Flatpak needs systemd, which WSL does not enable by default. In the distro:

    sudo tee /etc/wsl.conf >/dev/null <<'EOF'
    [boot]
    systemd=true
    EOF

then `wsl --shutdown` from PowerShell and start it again.

**Check Vulkan before anything else.** If this does not name the GPU, stop and
fix it first — every later failure will be blamed on the renderer.

    sudo apt update
    sudo apt install -y vulkan-tools mesa-vulkan-drivers
    vulkaninfo --summary

On WSL2 with the NVIDIA driver installed on the *Windows* side (not inside the
distro — installing a Linux NVIDIA driver in WSL breaks it), this should list a
device. `dzn` or `lavapipe` appearing instead means it fell back to a software
or D3D12-translation path, which will run but tells you nothing trustworthy
about real Vulkan.

## The build, in order

Everything below wants these:

    sudo apt install -y build-essential cmake ninja-build git python3 \
        libsdl3-dev patchelf pkg-config

If `libsdl3-dev` is not in 24.04's archive yet, build SDL3 from source — it
must be 3.x, because the code uses `SDL_OpenAudioDeviceStream` with a callback,
which SDL2 does not have. The Mac used 3.4.14; matching it avoids a variable.

**1. .NET 10.** Microsoft's apt feed, or the install script into `$HOME`, which
needs no root:

    curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0
    export PATH="$HOME/.dotnet:$PATH"

**2. Dawn.** The long pole in wall-clock terms, though far less than
`docs/macos-handoff.md` feared — it took 3m41s at 745% CPU on the Mac.

    ./build-dawn.sh

That fetches Dawn into `vendor/dawn`, builds it, and installs to
`vendor/dawn-install`. Both are gitignored. On Linux it will also want Vulkan
headers and X11/Wayland development packages; add what it asks for rather than
guessing up front.

**3. The renderer.**

    ./build-renderer.sh

Note this uses `-S renderer`, not `-S .` — there is no root `CMakeLists.txt`,
and `docs/macos-handoff.md` was wrong about that until the Mac session fixed
it.

**4. The key tables and the water.** Neither ships in the repo.

    git submodule update --init ffxi-engine
    python3 tools/keytables.py

    # LandSandBoat, for the compression tables and the water source
    git clone --depth 1 https://github.com/LandSandBoat/server.git ~/LandSandBoat
    cd ~/LandSandBoat && git submodule update --init --depth 1 ximeshes && cd -
    python3 tools/makewater.py --root ~/LandSandBoat/ximeshes

`ximeshes` is a submodule of LandSandBoat and a plain clone leaves it empty —
that cost the Mac session a detour. It produced 185 zones of water in 30
seconds, about 55MB.

**5. The package.**

    tools/package-linux.sh --res ~/LandSandBoat/res --zone-data ~/LandSandBoat/data/zones

**6. The Flatpak.**

    sudo apt install -y flatpak flatpak-builder
    flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathubrepo.flatpakrepo
    flatpak install --user flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08
    flatpak-builder --user --install --force-clean build-flatpak flatpak/com.tagban.MogHouse.yml
    flatpak run com.tagban.MogHouse

## Prove the renderer before the client

The standalone renderer is the cheaper thing to debug, and it needs no Flatpak.
Note that `--frames 1` is **not a flag** — `renderer/main.cpp` reads `argv[1]`
as the zone path and takes everything else from the environment, and ignores
unknown arguments silently, so it will just sit in its interactive loop. The
one-frame check is:

    MOGHOUSE_FFXI_KEYTABLE=keys/mzb_key_table.bin \
    MOGHOUSE_FFXI_KEYTABLE2=keys/mmb_key_table2.bin \
    MOGHOUSE_FONT=renderer/assets \
    MOGHOUSE_SUBROOMS=renderer/assets/subrooms.txt \
    MOGHOUSE_SCREENSHOT=/tmp/shot.png MOGHOUSE_SCREENSHOT_AFTER=8 \
      ./build-renderer/moghouse-renderer "<path to a zone DAT>"

A clean run prints an adapter line, a zone line, writes the PNG and exits 0.
On the Mac that read:

    adapter: Apple M4 (Metal)
    window: 1280x720 points, 2560x1440 pixels
    zone f_sa: 41178 triangles

On Linux the adapter line must say **Vulkan**. Zero `webgpu error` is the bar —
all four WGSL modules compiled on Metal with no complaint, which is mild
evidence they are portable, but Vulkan's validation is stricter in different
places than Metal's.

## What is most likely to break

Roughly in order, and honestly flagged rather than predicted.

**Vulkan device selection.** Dawn finding no adapter is the most likely single
failure, and on WSL2 it is usually the driver rather than the code. Check
`vulkaninfo --summary` first, every time, before reading any renderer source.

**A read-only `/app` surprise this fix did not cover.** The config and log
paths are handled, but anything else that writes relative to the executable
would fail the same way. If something dies on first run inside the Flatpak and
works outside it, this is the shape to look for.

**Audio.** SDL3 opens a stream for zone music. The manifest grants
`--socket=pulseaudio`, which covers PulseAudio and PipeWire's Pulse shim. WSL2
has no audio device at all by default, so expect audio to be absent there and
do not read that as a bug — test it on real hardware.

**Fractional scaling.** The Mac had a genuine points-versus-pixels bug that was
invisible until real geometry was on screen: the surface was configured in
points while the drawable was in pixels, so everything rendered at half
resolution and a flat clear colour looked identical either way. It is fixed
now, in a portable way — `SDL_GetWindowSizeInPixels` plus
`SDL_WINDOW_HIGH_PIXEL_DENSITY` — but Linux under fractional scaling is the
other place that split exists, so it is worth confirming rather than assuming.
The `window: WxH points, WxH pixels` line the renderer prints is the check.

## Two bugs from the Mac that are not macOS bugs

Both are already fixed, and both are worth knowing because the same class of
thing will happen again.

**`viewer.cpp` had a conditional whose branches were a `const char*` and a
`bool`.** MSVC accepted it through pointer-to-bool and computed the right
answer by accident; Clang rejected it outright. GCC will likely reject it too
if anything similar remains. The lesson is that "it compiles on Windows" is not
evidence that it is valid C++.

**The renderer segfaulted on every exit.** `~Music()` destroyed an SDL audio
stream *after* `SDL_Quit()` had already torn the audio subsystem down, so it
locked a freed mutex. That is undefined on every platform — Windows simply
survived it. Linux may or may not. If the renderer crashes on quit with a
stack in SDL, this is the shape, and the fix pattern is
`Music::shutdown()` called explicitly before `SDL_Quit`.

## The thing this project keeps relearning

From `docs/macos-handoff.md`, and it earned its place again on the Mac: **two
halves of one client agreeing tells you they were written by the same person,
not that either is right.** Validate against something you did not produce.

It bit once more during the macOS packaging, in a new disguise. The first
x86_64 bundle was built with an **arm64 SDL3 inside it** — because the lookup
fell through to Homebrew's copy, which only exists for one architecture. It
signed cleanly. It verified cleanly. `codesign --verify --deep --strict`
passed. Every check the build could run on itself agreed, and the bundle could
not have loaded on the only machines it was built for.

The fix was not a better lookup — it was adding a check that reads the answer
off the artifact rather than trusting the process that made it:
`package-macos.sh` now runs `lipo -archs` over every binary and refuses to
package a mixed bundle. `package-linux.sh` does the same with `file`. Keep
that check. The build machine is always the one architecture that happens to
work, so this is exactly the bug that cannot be caught by running it locally.

## Where the release stands

macOS is finished: signed, notarized, stapled, verified as a downloader sees
it, for both arm64 and x86_64. `tools/package-macos.sh` builds them and
`tools/notarize-macos.sh` submits and staples.

Linux has no equivalent yet. When it works, the natural next step is putting
the Flatpak build in CI — free x86_64 runners, reproducible artifacts, and no
dependence on any one machine being booted into the right OS. The VM then
becomes a test environment rather than a build environment, which is the
healthier split. CI cannot test the GPU, so a real machine still has to answer
the Vulkan question at least once.
