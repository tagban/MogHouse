# What is not done yet

Kept in one place because the list is now longer than anyone will remember,
and because half of these were found by playing rather than by reading code.

## Loading

Zoning tears the world window down and builds a new one, so there is a gap with
nothing in it. That gap should say something.

- **A loading screen.** A bouncing moogle would do, and the moogle is in the
  DATs - creature models already render, so it could be the real one animating
  rather than a picture of one.
- **Eventually: the destination zone's map, and its name.** A top-down image is
  already baked per zone, so this is a question of *when* rather than *how*:
  either bake the destination early in its own load and show it while the rest
  finishes, or pre-generate every zone's map into assets and show it instantly.
  The second makes the client bigger and the loading screen honest about
  progress; the first costs nothing on disk and cannot show the map until it
  exists.

## The map

- **The full-screen map.** No map DATs needed: the baked top-down image, the
  zone lines and their destinations all exist already. Zone lines drawn as
  markers, each labelled with where it goes.
- **Movement reads mirrored on the minimap** when walking, though the compass
  agrees with a retail client and the bake measures 99.1% against the zone's own
  geometry. Isolate by pressing M for north-up: if it reads correctly then, the
  rotation's sign is wrong; if not, it is something else again.

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
- **206 of 439 model ids** land on files with no skeleton - probably a second
  expansion-era range.

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

## Done today, for scale

Building interiors and their collision and lighting; water from the terrain
material; zone music from the raw ADPCM; HP/MP/TP on screen; GM teleports;
every chat channel; the launcher handoff; auto-run; a rotating minimap;
settings that persist; and a test suite that had been quietly red.
