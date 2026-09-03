# Handing back from Windows

Written on Windows, 2026-09-02, after the session that took the Mac's
in-renderer screens and ran them here for the first time. Everything below is
on `master`, which is the branch - there is no `main`.

The previous version of this file was the Mac's brief to Windows. Its open
questions are answered here; what it parked is still parked at the bottom.

## What happened, in one paragraph

The Mac's work builds and runs on Windows with MSVC, and most of it worked
first time. Four things did not, none of them Windows-only in the end: the
renderer never saw the variables the client set for it, every expansion zone
was "not installed", a character built after sign-in never animated, and the
client froze on zone-in whenever the zone's music decoded. All four are fixed
below. Then Avalonia was deleted, the HP/MP/TP display became bars in the
bottom right, and character creation got dropdowns with a live figure. A
character was made, named and zoned in with all of it.

## Fixed this session

### Variables set from C# never reached the renderer on Windows

`NativeEnvironment.Set` assumed `Environment.SetEnvironmentVariable` was
enough on Windows because it reaches the Win32 process environment. It does,
and the C runtime's `getenv` does not read that: it reads a private copy taken
at startup, updated only by the runtime's own `_putenv` family. So
`MOGHOUSE_NATIVE_DIR` and `MOGHOUSE_LOG` were invisible to the renderer -
which is why the renderer's log listed every asset folder it looked in except
the one the client had just told it about.

It was hidden in development because the renderer also looks under the current
directory, and `dotnet run` from the repository root finds `renderer/assets`
there. A packaged build keeps the assets in `data\`, where only the variable
points, so **every packaged Windows build so far shipped with no HUD and no
renderer log**. `NativeEnvironment.Set` now calls `_wputenv_s` in `ucrtbase`
on Windows as well as `setenv` on Unix. Verified by starting from the `bin`
folder with nothing set: the atlas loads and `moghouse.log.renderer` appears.

### Expansion zones were "not installed"

All three file-table readers - C#, C++ and `tools/filetable.py` - read only
`VTABLE.DAT`/`FTABLE.DAT` at the install root, which describes the original
game. Each expansion has its own pair under `ROM2/VTABLE2.DAT` and so on up
to `ROM9`, covering the same id range, and later ones override earlier. Zone
123's map is file 223, which is in ROM2, so `!zone 123` said Yhoator Jungle
was not in the installation. All three now overlay the expansion tables, and
there are tests for it. This also explains some of the README's "some creatures
have no model" - the renderer's own reader had the same gap.

### A character built after sign-in walked without moving

The renderer looked up the idle, walk, run, jump and death clips once, before
its loop, from the character given at startup. With sign-in inside the window
the character arrives later, so every clip pointer stayed null and the body
held its bind pose while it moved; the upper-body lookup was bound in the same
startup-only block, so after the first fix the legs walked and the arms hung
still. Both are now bound again when a body is built after a zone load. This
was not Windows-specific and the Mac would have shown it too.

### Frozen on zone-in: a lock-order deadlock in the music player

`Music::play` held its own lock while calling `SDL_SetAudioStreamFormat`,
which takes SDL's stream lock. SDL's audio thread takes the stream lock and
then calls `feed()`, which takes ours. Two locks in opposite orders: the
render thread and the audio thread each waiting on the other, the window not
responding, the session thread happily asleep in `WorldLoop.Pump`. It only
fired for tracks that decode, which is why the jungle - whose track is "not a
BGW" - never hung and the cities did. The SDL call now happens outside the
lock. Found with `dotnet-stack` showing the main thread inside
`NativeViewer.Run` and nothing managed waiting; the native side was reasoned
out from there. The call was added by the Mac's sample-rate change, so the Mac
has it too.

### Smaller

- **The character went white after a zone change.** `readZone` clears the
  texture cache; the body was only rebuilt on the first zone. It is now
  rebuilt from the look it was built from after every zone load.
- **The face did not change the head.** The body cache keyed on race and
  gear and left the face out, so every face wore the first head built. The
  face is a separate DAT, hair and all, and is in the key now.
- **`zone: nothing arrived within 0.4s` filled the log**, several lines a
  second in a quiet zone. It now reports once when nothing has arrived for
  five seconds, and once more when the server speaks again.

### Southern San d'Oria was one shade of teal

Two faults together. The frame chose "indoor" lighting from the camera's
position rather than the character's, and the camera hangs back into the
building behind the character in a narrow street. And four of the zone's
sub-files - districts, not buildings, up to 1496 units across - carry lighting
of their own and were treated as rooms, so the character was always "inside"
one and its set lit every street and the sky. The room test now uses the
character's position and ignores any box wider than 150 units. The log says
which room's lighting is in use whenever that changes.

### Smaller, second round

- **A dropdown let the rows beneath it show through.** The dialog shader
  drew every row's text after every rectangle; text under a later opaque
  rectangle is now skipped.
- **Size scales the body.** The look string carries an eighth number, the
  server's `char_look.size` travels through the tracker and the C ABI as
  `MhRadarEntity.size`, and bodies are drawn at 0.92, 1.0 or 1.08 - an
  approximation, not the retail client's factors. The creation figure and
  everyone in a zone both honour it.
- **A sliding NPC** - the San d'Oria delivery NPC, per the report - is most
  likely a body whose clips are not named `idl0`/`wlk0`/`run0`. The renderer
  now prints such a body's clip names once, so the next log will say what
  they are called and the gait code can learn them.

### Third round: rooms, the flash at doorways, and a dozen of you

- **Floating buildings in Southern San d'Oria were shop interiors.** The
  game keeps its interiors in the sky above the city and draws only the one
  you are in; this drew them all. Every room now records its draw range, and
  a room narrower than 150 units is drawn only while the character is inside
  its box or within six units of it. Rooms are also left off the minimap
  bake, which is what had painted their roofs over the whole plan and made
  the map read as one green blob.
- **Lighting flashed at doorways.** The set now fades over 0.6s from the one
  in force to the new one, sky and fog included.
- **A dozen copies of the player.** An entity whose look cannot be built was
  drawn as a copy of the player's body. It is now the pale blank shape the
  character-select figure uses. The looks in question are race 29 to 31,
  which are the child NPCs - the San d'Oria delivery girl among them - and
  the renderer only knows the eight player races' model files. As a
  stopgap a child race is now built as the grown race - 29 Mithra, 30 Elvaan
  woman, 31 Hume woman - at 68% height, with the roster's stand-in clothes
  and the face clamped into the adult range, so she is dressed, walks and
  no longer slides. Deriving the children's own bases the way
  `tools/pcmodels.py` derived the adults' is the real fix; the equipment
  index carries no tag for them, so it needs a different route in.

### White scenery in the sky over San d'Oria

Reading the expansion file tables made seven more sub-files loadable for
Southern San d'Oria that had silently failed before. A per-room count added
to the load log showed three of them - `ROM2/15/124`, `ROM2/21/115` and
`ROM2/21/116` - with every one of their 707 draws naming a texture this zone
does not hold: a 113-unit castle ornament in the sky and two 256-unit halls,
all white. They are struck from `renderer/assets/subrooms.txt` with a note.
The four `ROM3/7/49..52` entries are the zone-sized districts and stay. The
two draws that name no texture at all - an arch over a stair, drawn as a
cream shell - are the game's occlusion volumes and are no longer drawn.

### Who the server says not to draw

A retail client side by side showed nobody where MogHouse drew a row of pale
knights on the castle steps, and a body in every doorway. Two causes, both
in the entity update and both confirmed against the server's generated
enums (`build/generated/data/enums/status.h`, `entity_flags.h`,
`name_vis.h`) and its zone data:

- The **status byte at 0x20** was never read. 2 Disappear, 3 Invisible and
  6 CutsceneOnly are not drawn by the retail client; 366 of Southern San
  d'Oria's NPCs are `cutscene_only`. The tracker now hides on those and on
  the entity-flags HideModel bit (0x80), sticky across position-only updates.
- **Entities with no look at all** - doors, triggers, markers - were drawn
  as stand-in bodies. They are drawn as nothing now.

The whole table is in `docs/wiki/Entity-Visibility.md`. The tracker logs one
line per NPC on first sight with its look, status, flags and whether it was
hidden, which is how the rest of this was worked out.

Also from that comparison: a zone-handshake failure straight after a refused
entry used to end the whole session; it now returns to character select with
a message.

### Port Bastok's harbour stood above the quay

The water surfaces still come from the server's collision meshes
(`tools/ximesh.py`), where the harbour is 6,144 water triangles at one
height half a unit under the quay: the sea's own surface, not a bed. The
waterline tool lifted it a unit above its "bed" the way it lifts a canal's
flat floor, which put the harbour over the dock. A large pool whose bed is
four fifths one height is now taken as a sea at that height, lifted only
enough not to z-fight. Every zone was regenerated; the files are still not
in git. The minimap no longer draws water at all - a harbour plane baked
from above was the green disc Port Bastok's map had become.

The ripple is the zone's own: FFXI's water is an alpha ripple sheet the
client scrolls, and the water pass has always scrolled one - but it looked
the sheet up by its full sixteen-byte name under the "effect" group, and Port
Bastok keeps its harbour sheets (`umi2`, `sea01`, `miz1`, `miz2`) under "sea"
or under no group at all, so the harbour got the plain fallback and no
ripple. The pass now matches on the texture's own name whatever the group,
preferring the sea sheets on open water.

**Water from the DATs themselves is the request** and remains open: the
colour and darkness of retail's water in the side-by-side are a shader
matter, and which ground is underwater is still read from the server's
meshes rather than the zone file - see `docs/water-candidates.md` and the
water memory for what was tried. The per-cell height in the MZB is written
as a sentinel in some zones, so it cannot simply be trusted; matching the
MZB's own triangles to the server's by geometry is the route not yet taken.

### Overnight, from the list left at bedtime

- **Sign-in has a dropdown of saved servers** (`LOAD`), using the new
  Choice row; picking one fills the fields in. The cycling button is gone.
- **The clock runs.** The sign-in backdrop is held at 17:00 on purpose and
  that pin used to follow the player into the world. It now holds only until
  the server's clock arrives; `MOGHOUSE_TIME` in the environment still pins.
- **Chat is one box**, a fixed panel the height of the log rather than a chip
  behind each line, drawn with the HUD's bars array.
- **Water reads as the sea where it is the sea.** The ripple sheet is matched
  by its own name whatever group it sits in (Port Bastok's are under "sea" or
  none), the sea sheets darken the body to near-black, and a grazing-angle
  mix of the horizon colour stands in for the sky's reflection. Compared to
  the retail client at the same quay: close in tone, no real reflection.
- **The minimap** was Sel Phiner's map in every zone entered from the sign-in
  screen: the bake made a new texture per zone while the radar stayed bound to
  the first. The texture is made once and baked into again, and the walkable
  mask is rewritten with each bake.
- **NPC flicker:** the tracker now logs every look change per entity. None
  seen yet in Port Bastok or East Ronfaure after several minutes each.
- **East Ronfaure's stream**, from the water file: it descends in ten-unit
  terraces (59, 49, 39, 29, 19, 9) with flat pools between. Whether that is
  the real riverbed's shape or an artefact of the local waterline needs the
  retail client beside it; `!pos` to the stream from our client did not move
  the character, and that is its own open question - `!zone` works, so it
  is not permissions.
- **GM teleports now move the character.** `!pos`, `!goto`, `!bring` and
  `!up` all come down to the server's `setPos`, which sends packet 0x05B;
  the client read only 0x065, whose layout is identical, so every teleport
  moved the character on the server and nowhere else and the client's own
  position reports put them straight back. Both ids are accepted now.
  `FfxiServerPosition.cs` says so. This is also why `!up` never got anyone
  off the tunnel roof.
- **East Ronfaure's stream**, stood in it: the surface is at knee height on
  the bank at 262,44, so the local waterline is a little high there, and the
  terraces are ten units apart the whole way down. That wants the retail
  client beside it, which had been closed by then.
- **Not started:** effects (airship landing, raise/death), sound effects,
  the Port Bastok airship bridge, weather. What is known: the weather
  packet is 0x057 with a start time, a weather number and an offset; the
  client has no weather layer at all, and the sky is a gradient with no
  clouds. Sound effects have a decoder (`tools/spwdecode.py`) and no player
  channel. The bridge is a placement, movable the way the monorail is, once
  its model name is found in Port Bastok's placement list. Effects are
  unresearched; the game's effect files are separate DATs the client does
  not yet parse.

## Changed this session

### Avalonia is gone

`MogHouse.App` is `Program.cs` and a project file. `Main` starts the log and
hands the main thread to `ClientFlow.Run`; `--screens` is accepted and
ignored. What the old shell set up that the screens path had not was moved
into `ClientFlow`: the zone-data and navmesh folders (environment or beside
the executable), the install's file table for NPC dialogue, and the session's
status lines going to the log. Verified: zone lines load, the status lines
appear, sign-in through to the world works with no flag.

The chat panel and the flat radar the Avalonia window had are already in the
renderer and were not ported. One thing was lost: a link in chat could be
clicked to open it. The renderer's chat cannot yet.

### HP, MP and TP are bars, bottom right

Three meters with the numbers written on them, in the corner nothing else
used. The HUD shader gained a small array of filled rectangles for it. They
were text in the bottom left before, where the chat log grew over them and
hid the TP line first. HP goes amber at a quarter and red at zero, TP goes
gold at a thousand.

### Character creation has dropdowns and a live figure

A new form row kind, `Choice`, in the renderer and across the interop: one box
reading `CAPTION: OPTION v` that unfolds into a list when pressed. Arrow keys
step it, return unfolds it, tab moves on. Picking hands the form back at once
with the row as the button, so the client can react before anything else is
pressed. The form can also stand aside - against the left edge, world left
bright - through `mh_viewer_set_form_aside`.

Creation uses both: race, gender, face, hair, size, job and nation are
dropdowns, and the character stands in the world beside the screen looking
the way the choices say. Race, gender, face, hair and size all change the figure.

### NPCs glide between updates

They eased to each server position in a tenth of a second and then stood
still until the next, which read as step-pause-step. Each new position is now
walked to over as long as the previous one took to be replaced, so a body
arrives about when the next update does and is always moving while its owner
is. One update behind the server, invisibly. **Not yet judged by eye** - it
was built at the end of the session and needs someone to stand near a walking
NPC and say whether it is better.

## Handoff, end of 2026-09-03 (read this first)

The next session is a different model; the long section after this is the
day's narrative and the wiki pages carry the formats.

**State.** Everything is committed on `master` (last commit: harbour water
back, nested curves, this handoff). Nothing pushed today. Tests 146 pass, 12
skipped. LandSandBoat was running at session end (four exes from
`C:\Users\Gaming\Desktop\LandSandBoat`). Jerk (GM) is in Bastok Markets.

**Verified against retail today:** water from the zone's own meshes placed by
effect generators; fountain jets (on by day) and lamp flames (on by night)
from the 0x19 curves; deep blue night sky from the fine-weather lighting set;
cloud dome and stars around the camera; the auction house as the stone
building (tent halves struck in `assets/hidden-models.txt`); stairs walk as a
ramp; harbour water rippling beside the bridge.

**Open, in the user's order:**

1. Lamp glows and the fountain's big flames staying lit by day - fixed at
   the very end: the generator length byte's top three bits are flags, and
   read whole it made op 0x12 look 164 words long and dropped the curve
   after it. `MOGHOUSE_LIST_GENERATORS=1 ffxi-datdump` now shows `bll1 ->
   fflt`, `bfl1 -> frtm`, `gl01 -> lttm`. **Not yet judged by eye** in the
   client; if anything is still lit at noon, check its curve name there.
2. Smoke (`bsmk`) is a still one-frame sprite; retail rises it as particles.
   The particle opcodes (0x15 box, 0x07 on the foam, 0x2d life curve, 0x13)
   are the next format.
3. Sprite frame rate is a guess at 10/s; sprite size uses op 0x0f and looked
   right on flames and lamps.
4. Weather: read packet 0x057, pick `weat/<x>` objects and lighting to match.
5. Sun and moon spheres not drawn (untextured, `k000` curve).
6. From earlier: fish/birds, nation flag at creation, child race models,
   clickable chat links, standalone renderer crash, meaning of op 0x27.

**Traps that cost time today:** LandSandBoat holds a killed session ~60 s
(relaunch inside it -> `checksum FAILED`, wait it out); a Bash heredoc
collapses doubled backslashes in patch scripts and a very long heredoc gets
cut (write scripts with the Write tool); PowerShell here-strings are CRLF
and will not match LF source; a Python string with `C:\U...` needs a raw
string; generator model ids collide (`auc_`) - resolve by directory; 0x1f
meshes are not for drawing; the sky generators share opcodes with the
effects and are filtered by `weat`; hiding things by an unknown opcode
(0x27) took the harbour away.

## 2026-09-03: water from the DATs, and the first effects

The overnight water work was thrown away in the morning, for the right reason:
the user asked how retail places its water, and the answer is in the DAT. The
water models every zone carries but "nothing references" are placed by the
**effect generator chunks** (type 0x05, in the `effe` directory), which also
place the fish, the birds, the fountain jets and the torch flames.
`docs/generator-format.md` has the format; `renderer/ffxi/generator.cpp` reads
it; `tools/generators.py` and `MOGHOUSE_LIST_GENERATORS=1 ffxi-datdump` list a
zone's generators.

What changed:

- `mh::isWaterMesh` marks a mesh as water by name or by ripple sheet texture
  (`kaw1`, `sea01`, `ike1`...). **The stream's meshes carry the blend flag**;
  an early version excluded blended meshes and found nothing.
- Water meshes go to the water pass as world-space triangles with their own
  UVs (`scene.cpp`), placed by the generators through `placementTransform`.
  The collision-derived `.water` sheets load only when a zone has no water
  mesh. East Ronfaure's stream (36 pieces on a 40-unit grid, sloping -48 to
  -7) and Bastok Markets' canal, fountain basin and sea all verified by eye
  and by the server's own height at 262, 44.
- The sheet the water pass scrolls is the one the zone's water meshes name,
  voted by triangle; a zone whose water is mostly untextured (Bastok's canal)
  is tinted as a river even though its harbour names `sea01`.
- Generator models with a texture animation (op 0x63) are placed as **effect
  draws**: `effectPipeline` (`renderer/effect_shader.h`) scrolls their texture
  at the generator's op 0x28 rate and skips night-only ones (op 0x0d - the
  fountain flames and glow) between 06:00 and 18:00. Judged against retail at
  20:35 the same evening: the jets read right ("sprinklers are definitely
  working"), retail's jets are indeed off at night (no flag found for that
  yet - ours stay on), and retail's lamps are lit. Ours are not, because the
  flames are not meshes: the flame generators name `hi12` and the glow names
  `lt`, which are textures for camera-facing sprites, and `hi12` is not in
  the zone DAT at all but in some shared effects file. A billboard pass is
  the next step. The weather directory places the moon and stars with the
  same opcodes - they drew as grey triangles until effects were limited to
  the `effe` directory. Retail's night is also much darker and bluer than
  ours. The 0x19 keyframe chunks (`watm`, `frtm`) the animations name are
  unread.
- LandSandBoat holds a killed character's session ~60s; relaunching inside
  that window makes the zone server answer with the dead session's key and
  the client logs `checksum FAILED`. Wait it out.

- **The sky, from the weather generators.** `weat/fine` in each zone places
  the cloud dome (`cld_fine_a01`, scale 18), the star field (`star`, 40 up,
  scale 3), the sun and moon spheres (untextured, not drawn yet) at the
  origin, meaning around the camera. Textured ones are built into
  `skyObjects` (their own buffers) and drawn by the effect pipeline between
  the sky gradient and the zone, camera-relative and unfogged
  (`effect.scroll.w = 1`). Clouds take the zone's light and drift; stars are
  night-only and drawn with `effectAdditivePipeline` - alpha-blended, the
  star sheet's black showed as dark triangles. Sel Phiner has no weather
  directory and borrows West Ronfaure's sky the way it borrows its lighting.
  Retail's night sky is still a deeper blue than ours: that is the lighting
  ramp, not the sky objects. Weather other than "fine" is not read yet.
- **Stairs.** Two fixes in `collision.cpp`/`.h`: a wall's height is judged
  from the tread being stepped onto (`footing`) rather than the floor stood
  on, and `kDefaultStepUp` went 0.95 -> 1.0 because the fountain plaza steps
  in Bastok Markets have a 0.96 riser in the server's own mesh (found from
  the position the user sent: server -249, -12.6, -116).
- **Flames.** The flame generators name `hi12`, which is a type 0x1f model
  with its own 0x20 texture and 0x21 animation in the shared effects file
  `ROM/0/0.DAT` (found by `scratchpad/findchunk.py`-style scan of ROM). That
  is the format to read next for the fountain's night flames and lamp fire.

## Running it on Windows

```powershell
dotnet run --project src/MogHouse.App/MogHouse.App.csproj --no-launch-profile
```

From the repository root, or with `MOGHOUSE_FFXI_KEYTABLE`,
`MOGHOUSE_FFXI_KEYTABLE2` and `MOGHOUSE_FFXI_RES` set - the key tables are in
`keys/` here and the compression tables in LandSandBoat's `res`. Set
`MOGHOUSE_FFXI_ZONEDATA` to LandSandBoat's `data/zones` for zone lines.

The renderer rebuild needs Visual Studio's own cmake through `vcvars64.bat`;
`tools/package-windows.ps1` does it that way, and so does the note in
`docs/where-things-are.md`. A build without vcvars fails silently and leaves
a stale DLL.

A local LandSandBoat lives at `C:\Users\Gaming\Desktop\LandSandBoat` with the
four servers as executables in its root; MariaDB runs as a service. The Mac's
server at 10.0.0.11 was reachable from here on the auth port but was not used.

`dotnet-stack` is installed as a global tool now. It shows managed frames
only, which was enough to place the hang in native code.

## Still open

**A nation flag behind the figure being made.** The conquest outpost banners
for all three nations are placements in nearly every contested zone, so one
could be moved behind the line-up spot the way the monorail is moved - only
placements that already exist can be moved, and these exist. Nobody has yet
found which placements they are.

**Standing on top of a tunnel after `!zone`.** The server placed the character
inside a tunnel and the client's ground search put them on its roof; `!up`
then appeared to do nothing because the client snapped back to the same roof.
The ground search should prefer the surface nearest the server's own height
rather than the first one found. `N` toggles collision off as a way out.

**Clickable links in chat**, as above.

**Child NPC races 29 to 31** have no model bases in `renderer/ffxi/look.cpp`,
so they are drawn as blank stand-ins and do not animate. Score their files
the way `tools/pcmodels.py` scored the adults'.

**The standalone renderer crashes at startup** on any zone, with exit
0xC0000409 and nothing printed, run either way `run.ps1` runs it. The
in-process renderer is fine. Whether this predates today is not known; the
executable was rebuilt with every change here. `shot.ps1` therefore does not
work on this machine until it is fixed.

**The 4K scaling question** from the Mac's brief could not be checked: both
displays here are 1080p at 96 DPI, where the ratio is 1.

**Parked from before, unchanged:** a seated pose while riding the monorail;
sound effects; Bastok Mines blowing out to white at noon.

## Where to look

| | |
|---|---|
| `src/MogHouse.Core/Interop/NativeEnvironment.cs` | the Windows C-runtime fix, with the explanation |
| `src/MogHouse.Core/Ffxi/FfxiFileTable.cs` | expansion tables; the C++ and Python copies say "change all three" |
| `renderer/music.cpp` | the lock order, and why it must stay that way |
| `renderer/viewer.cpp`, `bindClips` | clip binding for a late-built body |
| `renderer/viewer.cpp`, `FormRowKind::Choice` | the dropdown, drawing and input |
| `src/MogHouse.Core/Screens/CharacterScreens.cs` | creation with the live figure |
| `renderer/hud_shader.h` | the bars |
| `docs/where-things-are.md` | the map of the repository, updated |
