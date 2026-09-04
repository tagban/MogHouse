# Handing over to Windows — build 0.2.0

Written on the Mac at the end of 2026-09-03, for whoever picks this up on the
Windows machine. The job is one thing: **produce and publish the Windows half
of the 0.2.0 pre-release.** The macOS half is already out.

Everything is on `master`, pushed, and tagged `v0.2.0`. Nothing is local-only
this time.

## Where things stand

- **Released:** https://github.com/tagban/MogHouse/releases/tag/v0.2.0 —
  marked pre-release, carrying `MogHouse-XI-Alpha-0.2.0-macos-arm64.zip`
  (46 MB, signed and notarized, Gatekeeper accepts it).
- **Not released:** Windows, and macOS Intel. Both are missing for the same
  reason - each needs its own Dawn and SDL3, and this Mac has neither an
  x86_64 Dawn nor an x86_64 Homebrew. `vendor/dawn` is not even checked out
  here.
- 101 commits since `v0.1.2`, none of them compiled with MSVC.

## The job

```powershell
.\build-dawn.bat                # only if vendor\dawn-install is missing
.\build-renderer.bat
dotnet build MogHouse.slnx
.\tools\package-windows.ps1 -Version 0.2.0 `
    -Res "<LandSandBoat>\res" -ZoneData "<LandSandBoat>\data\zones"
```

Then attach the zip to the **existing** v0.2.0 release rather than making a
new one:

```powershell
gh release upload v0.2.0 dist\MogHouse-XI-Alpha-0.2.0-win-x64.zip
```

The version is already `0.2.0` in all three packaging scripts; it does not
need passing, but passing it costs nothing and documents intent.

## What MSVC has never seen

101 commits of it. The parts most likely to break a build clang was happy
with:

- **`renderer/ffxi/dialogue.cpp`, `soundrefs.cpp`, `sounds.cpp`** are new
  files. `sounds.cpp` opens an `SDL_AudioStream` per voice; if SDL3 on Windows
  refuses the default playback device the client must still draw - it is
  written to survive that, but it has only been seen surviving it on macOS.
- **`renderer/character.cpp`** gained `headBone` and `pitchHead`, which use
  `std::fabs` and `Mat4` multiplication in a loop over the bind pose.
- **Three interop signatures changed**, so a stale `libmoghouse_interop.dll`
  beside a fresh client will crash rather than misbehave quietly:

  ```
  mh_viewer_set_settings (+ float sound_volume)
  mh_viewer_take_settings (+ float* sound_volume)
  mh_viewer_push_chat     (+ int32_t tone)
  ```

  Rebuild the native side before the managed side, and if anything looks
  strange, check the DLL's timestamp first.
- **`kHudBars` went from 8 to 12** in `hud_shader.h`, and the WGSL hardcodes
  the same number in three places with a `static_assert` guarding it. If that
  assert fires, the shader and the constant have drifted.

## What to actually look at once it runs

The day's work is mostly things you have to see or hear:

1. **Chat.** There is a box at the bottom that is always there and says
   "Press Return to chat". Type in it. Lines should be coloured by channel -
   a tell pink, linkshell lime, party blue, an NPC gold. No `>` anywhere.
2. **Options menu.** The hamburger right of the clock, or numpad `-`.
   Music and Sound are separate and both survive a restart. **Numpad `-` no
   longer toggles walk/run - that moved to numpad `*`.**
3. **Sound.** Stand in West Ronfaure: wind always, and a waterfall that fades
   in as you approach. Console prints `ambience:` lines saying what it held
   and what it refused.
4. **NPCs turn to look at you** when you click them, head included.
5. **A popup at login** about `/bug`. It is modal, so it holds the keyboard
   until dismissed - if jump stops working, check nothing is on screen.

## Known limitations, so they are not reported as new

- **NPC cutscenes do not play.** The server says which event to run; the
  scripts are not decoded. An NPC with a cutscene reports its id and ends it.
  NPCs that merely speak do work. See `docs/wiki/Dialogue.md` for what is
  known and, more usefully, what has been ruled out.
- **Only the English dialogue is read.** `6120 + zone` is the same table in
  Japanese and is never opened.
- `chat_shader.h` is dead code - its pipeline is built and never bound. Chat
  draws through the HUD atlas. Worth deleting; it reads like live code.

## If the Intel Mac build is wanted

It needs `vendor/dawn` cloned and built with `CMAKE_OSX_ARCHITECTURES=x86_64`,
plus SDL3 built from source for x86_64, plus a third renderer tree. Hours, and
a cross-compile path this repo has never exercised. It was skipped for 0.2.0
deliberately rather than forgotten.
