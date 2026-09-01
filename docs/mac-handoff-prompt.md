I'm continuing work on **MogHouse**, a from-scratch Final Fantasy XI client, on macOS. It has been developed on a Windows machine; nothing has ever been built or run on a Mac. Your job is to get it building and running here.

**Repo:** `github.com/tagban/MogHouse`, branch `master`. Clone it, then read `docs/macos-handoff.md` first — it was written on the Windows machine specifically for you and is the authoritative brief. `docs/roadmap.md` is the honest list of what isn't done.

## What this is

A .NET 10 / Avalonia client with a C++ renderer (Dawn/WebGPU + SDL3) loaded as a native library. It reads a retail FFXI installation's own DAT files directly — zones, interiors, models, skeletons, animations, textures, music — and speaks the FFXI protocol to a private LandSandBoat server. It ships no game data.

A Windows alpha was released today: `v0.1.2`. It works — you can log in, walk around a city with real geometry, interiors, collision, water, music, NPCs and other players. There is no combat.

## Before anything else

The client requires an FFXI install on the **August 2026** patch and a server of that same version. It is **not** backwards compatible, and the failures are quiet rather than loud — file ids move between versions so the wrong model loads, packet layouts shift so fields are read from the wrong offsets. If you end up with a game directory copied from an older machine, that's the trap.

## The work, in the order I'd do it

1. **Build the renderer.** `cmake -S . -B build-renderer -G Ninja && cmake --build build-renderer`. Expect two fights:
   - **Dawn is the long pole.** `vendor/dawn-install` is a *Windows* prebuild with a static `webgpu_dawn.lib`. The Mac needs its own Dawn. Budget most of your time here.
   - **SDL3.** `vendor/SDL3-3.4.14` is the Windows binary distribution. Use Homebrew's or build it — it must be 3.x, since the code uses `SDL_OpenAudioDeviceStream` with a callback, which SDL2 lacks.

   If Dawn is fighting you, `ffxi-datdump`, `ffxi-collisiondump` and `ffxi-chardump` need no GPU and will prove the DAT reading works before the graphics do.

2. **Get a window up.** The likely afternoon-sink is the `CAMetalLayer` handoff between SDL3 and Dawn. Verify with one frame and count errors:

   ```
   MOGHOUSE_ZONE_NAME="Bastok Markets" ./build-renderer/moghouse-renderer <zone DAT> --frames 1
   ```

   A clean run prints an adapter line, a zone line, and no `webgpu error`. **If the window is black, suspect WGSL first** — a shader error takes down the entire module, not one line. Metal's compiler is stricter than D3D12's. There are four shader modules in `renderer/*_shader.h`.

3. **`FfxiInstall.Find()` needs a macOS branch.** It's registry-and-Program-Files shaped. On a Mac the game normally lives inside a Wine or CrossOver prefix. The install picker already handles being pointed at the right folder and now appears on first run, so the fallback path is fine — it's detection that's missing.

4. **An `.app` bundle.** `tools/package-windows.ps1` shows what has to travel with the client. Note that a bundle already has the right shape for it: `Contents/MacOS` for the executable, `Contents/Resources` for everything else. The hidden `data/` folder on Windows is a workaround for a problem a bundle doesn't have.

## Two things that will save you time

**Read the logs.** The client is a windowed app with no console. `moghouse.log` is the managed side, `moghouse.log.renderer` is the renderer. Two files because the .NET side holds its log open sharing for reading only. Everything the renderer prints — zones read, water loaded, models built, WebGPU errors — is in the second one. For a long time none of this was captured and bugs could only be reasoned about, which went wrong twice at length.

**Don't trust self-consistency.** This is the lesson the project keeps relearning and it cost the most time. The minimap was mirrored for weeks and passed every check, because the map bake, the sampling and the dots all shared the mirror and agreed with each other perfectly. Only the 3D camera disagreed. Validate against something you did not produce: the server's own numbers, a retail client, the game's files. `docs/wiki/Coordinates-And-Rotation.md` has the details — read it before touching anything positional. Short version: the world is `(x, -y, -z)`, a rotation not a reflection, and headings are `pi/2 - r`, not `pi - r`. Both mistakes have been made and both look plausible on screen.

## Practical notes

- Every data path falls back to a directory beside the executable, then a `data/` subfolder — no environment variables needed to run. The table is in `docs/macos-handoff.md`.
- `renderer/assets/water/*.water` is generated, not committed — ~50MB. Run `python tools/makewater.py`, which needs the server's `ximeshes` directory. Without it every canal and sea is dry.
- `dotnet test src/MogHouse.Core.Tests` — 143 tests, 12 skip without `MOGHOUSE_FFXI_RES`.
- The renderer's build needs the same care on any platform: a silent no-op leaves a stale library behind, so confirm the timestamp changed before blaming the code.

Start by reading `docs/macos-handoff.md`, then tell me what you find when you try to build Dawn.
