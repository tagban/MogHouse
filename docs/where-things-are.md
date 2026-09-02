# Where things are

Start here. The other handoff documents are chronological and long; this one is
short and current, and says what is true today rather than how it got that way.

Last updated after the monorail and the in-engine screens, 2026-09-02.

## The environment, which is the part you cannot guess

Nothing runs without these. The install is on an external volume, and the key
tables are built rather than shipped.

```sh
export PATH="$HOME/.dotnet:/opt/homebrew/bin:$PATH"
export MOGHOUSE_FFXI_INSTALL="/Volumes/AppStorage/FFXI Game Folder/PlayOnline/SquareEnix/FINAL FANTASY XI"
export MOGHOUSE_FFXI_KEYTABLE="$PWD/keys/mzb_key_table.bin"
export MOGHOUSE_FFXI_KEYTABLE2="$PWD/keys/mmb_key_table2.bin"
export MOGHOUSE_FFXI_RES="$PWD/dist/x86_64/MogHouse XI.app/Contents/Resources/data/res"
```

`dotnet` is not on the default PATH here, and neither is Homebrew's clang, which
is the one that builds the renderer - Apple's has no `std::jthread`.

## Running it

**The client**, screens and all:

```sh
dotnet run --project src/MogHouse.App/MogHouse.App.csproj --no-launch-profile -- --screens
```

Without `--screens` it starts the old Avalonia launcher, which is still the
default. Sign-in fills itself from the saved profile; tab to the password.

**The renderer on its own**, which needs no server and is how most of the
graphics work was done:

```sh
./build-renderer/moghouse-renderer "$MOGHOUSE_FFXI_INSTALL/ROM/0/28.DAT"
```

`ROM/0/28.DAT` is zone 0, Sel Phiner - the backdrop the client signs in over,
and the one with the monorail. `wasd` flies, `p` prints where you are, `t` gets
on and off the train, `escape` quits.

**The local server** is LandSandBoat at `/Volumes/AppStorage/LandSandBoat`, and
is usually already running. `pkill -f 'xi_map|xi_world|xi_connect'` stops it.
Two accounts, both `mhtestpw123`: `mhtest` (character Testy) and `mhtest2`
(Duo). A character that was logged in less than a minute ago cannot log in
again - the server holds the session for sixty seconds.

## Useful switches

| | |
|---|---|
| `MOGHOUSE_TIME=1700` | holds the hour still; the light is otherwise never twice the same |
| `MOGHOUSE_SCENE_ZONE` | the zone behind the sign-in screen; 0 is Sel Phiner, -1 is none |
| `MOGHOUSE_UI_SCALE` | interface size on top of the display's own correction |
| `MOGHOUSE_TRAIN_WATCH` | prints where the monorail has got to |
| `MOGHOUSE_TRAIN_HOLD` | leaves the train parked, which is the only way to walk its interior |
| `MOGHOUSE_TRAIN_LAMPS` | how brightly its cars light up |
| `MOGHOUSE_CLIPS` | lists a character's animation clips |
| `MOGHOUSE_CAMERA`, `MOGHOUSE_CAMERA_LOOK` | frame a shot from a script |

## What works

Sign in, make an account, choose or make a character, and zone in - all of it
drawn by the renderer in one window, over a live Sel Phiner, with the train
running past. Then the world: walking, chat, jumping, death and raise, zoning
between zones, vitals, music. `WorldLoop` in `MogHouse.Core/Screens/` is that
second half and is not user interface.

## What is next, in the order worth doing it

**Delete Avalonia.** The blocker is gone - the game logic that was trapped in
`GameViewModel` is in Core now. What remains is genuinely user interface: the
launcher window, the XAML views, the radar control, the observable properties.
The job is removing those, making `--screens` the default, and following it
through `MogHouse.App.csproj` and `tools/package-*.sh`.

**Bastok Mines blows out to white around noon and goes black at night.** An
underground zone lit by the outdoor day and night cycle; only the thirteen rooms
that ship their own lighting escape it. Not the same fault as zones shipping no
lighting at all, which is fixed. `MOGHOUSE_TIME=1200` shows it at once.

**A seated pose on the train, and sound effects.** Both written up with working
notes in `docs/windows-handoff.md`. The sitting clips are `si00` through `si21`;
the sound format is solved and on the wiki, and what is missing is a mixer
rather than a decoder.

## Where the rest is written down

| | |
|---|---|
| `docs/macos-handoff.md` | how the Mac build happened, and two wrong theories worth not repeating |
| `docs/windows-handoff.md` | what is parked for the Windows session, and what blocks deleting Avalonia |
| `docs/local-test-server.md` | building and running LandSandBoat here |
| `docs/networking-handoff.md` | a false lead, kept so nobody pays for it twice |
| the wiki | data formats: object mapping, and audio |

The commit messages are the other half of this. Where something surprising was
found - the split sample rate, the collision that does not move with the train,
the far plane fixed at 1.0 - the reasoning is in the commit rather than here.
