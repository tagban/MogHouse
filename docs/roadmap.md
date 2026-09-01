# What is not done yet

Kept in one place because the list is now longer than anyone will remember,
and because half of these were found by playing rather than by reading code.

## Loading

Done: the window stays up across a zone change and the zone is swapped inside
it, which turned out to be fast enough that the loading screen had nothing to
cover. The artificial hold is gone.

- **Eventually: the destination zone's map, and its name** - wanted as a
  per-zone screen rather than one shared picture. A top-down image is already
  baked per zone, so every zone gets its own without any art being drawn. The
  question is *when*: bake the destination early in its own load and show it
  while the rest finishes, or pre-generate every zone's map into assets and
  show it instantly.

## The map

- **The full-screen map.** No map DATs needed: the baked top-down image, the
  zone lines and their destinations all exist already. Zone lines drawn as
  markers, each labelled with where it goes.
- **Movement may read mirrored on the minimap** when walking. Unresolved, and
  worth stating carefully because the evidence conflicts. The compass agrees
  with a retail client and the bake measures 99.1% against the zone's own
  geometry - but those checks only prove the bake, `mapUv` and the dots agree
  with *each other*, which they do by construction: the dots are placed by
  comparing world positions against the same reconstruction the map is sampled
  with, so a mirror shared by all three is invisible to every test run so far.
  Deriving it from the camera says there is one: `lookAt` gives the 3D view a
  screen-right of `(-cos h, sin h)` in world (x, z), while the radar
  reconstructs screen-right as `(cos h, -sin h)` - exactly negated. If that is
  right, the fix is one sign on the radar's screen-x, and the compass and notch
  must not be flipped with it. Not applied: the last sighting was withdrawn
  ("they just don't have names"), and this is precisely the change that was
  made once before on a theory and had to be reverted.

## Water

Working again as of today: the file is named `Bastok_Markets.water` and the
name reaching the renderer is the one shown to the player, `Bastok Markets`,
so the lookup missed and every zone entered by zoning had no material water at
all. Only the named water models showed, which is why canals with no `water`
model over them were dry. 185 zones now have a water file, up from 6.

- **Read the material from the DATs instead of shipping it.** The right answer,
  and the one that removes ~50MB of derived data and the question of whether it
  should be redistributed at all. `readMesh` already reads the fourth uint16
  per triangle, and `MOGHOUSE_TRI_META` tallies the three candidate fields -
  but measured against LandSandBoat's own material counts for Bastok Markets
  (Stone 77.2%, Wood 13.3%, DeepWater 1.3%, ShallowWater 0.4%), none of the
  three matches: the two nibbles both decay smoothly, which is the shape of a
  count rather than a material, and the spare index bits take only four values.
  Identifying it properly means matching triangles between our MZB parse and
  the server's mesh by geometry and correlating from there.
- **Flowing water.** Fountains and the Bastok canals read as moving in retail;
  the effect chunks (`0x05` generators, `0x1F` definitions) are the likely
  source, and the same work would light the telepoint crystals.

## Rendering

- **`0x1F` chunks.** The Homepoint and Telepoint crystals are built from these
  plus `0x05` effect generators, not from MMB models - which is why `warp07`
  has no crystal and every telepoint in the game is invisible.
- **Interior lighting for the outdoors.** Rooms carry their own times of day and
  now use them; the zone's own set is still applied flat.
- **Water's hard edges.** Retail blends into the bank; ours cuts a polygon
  boundary.
- **Creatures float about 1.06 units.** Suspect the root bone's rest
  translation counted twice.
- **Worms spawn underground** and surface when a player comes near, which is
  their real behaviour rather than a bug - but nothing here plays the emerging
  animation, so they sit buried instead.
- **206 of 439 model ids** land on files with no skeleton - probably a second
  expansion-era range.

### Hair comes out the wrong colour

Pipira is `face = 2, race = 6` in `char_look`, and renders with reddish hair
where a retail client shows blue.

What is established: `look.face` is a single value 0-15 - LandSandBoat's
`login_helpers.cpp` rejects anything above 15 with the comment `// Face 8B`, so
it is eight faces times two variants, and the variant is the hair. The head
window is 32 entries at offset 8 from the race base, and the two Tarutaru sexes
share one base, so those 32 have to cover more than one thing. The file we load
for face 2 is `ROM/46/100`, whose texture is a Tarutaru face with reddish-brown
hair - which is what is on screen, so the load is doing what it is told.

What is not established: the index. Only sixteen of the thirty-two entries
resolve to distinct head files, which is exactly 8 x 2, so something else has to
select between the sexes that share the base. Feeding the raw face value in
lands somewhere plausible enough to look like a character and not be one.

Worth doing properly with the textures side by side rather than guessed at.

## Protocol

- **`HIDE_MODEL` and `UNTARGETABLE`** are parsed and not acted on. Wiring them
  into what gets drawn means widening the interop struct, which is worth doing
  deliberately rather than folded into something else.
- **`r_1t`** - 32 files, 21,530 placements, the largest interior family in the
  game and still not attributed to a zone.
- **Sound effects.** `0x3D` SeSep chunks. The music codec is known now, which
  may or may not help.

## Client

- **Chocobos and mounts.**
- **A hand cursor** over the corner links.
- **Logos rather than text** on those links, which needs an image path for HUD
  elements - there is only a font atlas today.
- **The world window keeps SDL's default icon.** `SDL_SetWindowIcon` wants a
  decoded surface and `.ico` is not one; a small PNG beside it would do.

## Stairs

- **A Tarutaru catches on some of Bastok's stairs.** `kDefaultStepUp` is 0.95,
  chosen as the tightest value that clears the 0.9 risers that were measured -
  a margin of 0.05, and there are evidently stairs on the other side of it. The
  comment on that constant already names the real fix: one global height cannot
  tell a stair from a railing, because the difference is not height but that a
  railing is too thin to stand on. Raising the number alone brings back
  climbing onto bridge railings, so the width test is the work.

## Done today, for scale

Building interiors and their collision and lighting; water from the terrain
material; zone music from the raw ADPCM; HP/MP/TP on screen; GM teleports;
every chat channel; the launcher handoff; auto-run; a rotating minimap;
settings that persist; and a test suite that had been quietly red.
