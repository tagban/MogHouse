# Effect generators: water, sky, weather and effects in a zone DAT

The type `0x05` chunks of a zone DAT. Read from the retail files on
2026-09-03; every offset here was checked against East Ronfaure
(`ROM/0/121.DAT`) and Bastok Markets (`ROM/1/35.DAT`). See also [[Water]] for
what the water ones do and how they were found, and `docs/generator-format.md`
in the repository for the developer notes.

## Where they live

A DAT is a tree of chunks: type `0x01` opens a directory, `0x00` closes one
(`docs/mzb-format.md` has the container format). A city zone's tree looks
like this, with the directories that matter for this page in bold:

```
t_ba                      the zone
  fses / fser             sound (0x3D) and their generators
  mode                    the MZB (0x1C), the terrain textures, the lights
    hato, ligh/...        pigeons; lights by time of day
  weat                    **the weather**
    clod / fine / mist / suny     one directory per weather type
      sun1 cld1 cld2      generators: sun, two cloud layers
      star/ moon/         sub-directories: star field, moon
      lf01..lf03          lens flare textures and their animation
      indo                a 0x3D chunk - indoor variant
  effe                    **the effects**
    kawa                  the river (East Ronfaure)
    sea, seaa, funs       harbour, harbour surface, fountain (Bastok)
    sakn                  the fish schools
    flag, aose, aotr...   flags, other animated set pieces
  door                    schedulers (0x07) for door open/close
```

Models (MMB, type `0x2E`) and textures (type `0x20`) sit in the same
directory as the generators that use them. Type `0x19` chunks beside them
are keyframe tracks named by the generators (`watm`, `frtm`, `tkwa`), and
type `0x21` chunks are texture animations.

The MZB's placement table places only the terrain and the buildings.
Everything in `weat` and `effe` is placed by a generator.

## The chunk

Offsets are from the start of the chunk including its sixteen-byte header.
The chunk's own four-character id is the generator's name (`ka01`, `fnmz`).

| offset | field |
|---|---|
| 0x40 | 3 floats, 1.0 |
| 0x50 | 4 floats, 1.0 |
| 0x80 | 4 × `u32`: offsets of four opcode streams |

Each stream is opcodes until an op `0x00`:

```
u8  op
u8  length in 4-byte words, header included
u16 zero
... payload
```

The first stream holds ranges (`0x0a`, `0x15`); the second places the model;
the third holds fades and flags; the fourth is usually empty.

### Opcodes read so far

| op | words | payload | seen on |
|---|---|---|---|
| `0x01` | 12 | +4 flags, **+8 model id** (4 chars), **+16 position x y z** | everything that places a model |
| `0x09` | 4 | rotation x y z, radians | |
| `0x0f` | 4 | scale x y z | |
| `0x63` | 4 | +4 texture animation id (4 chars) | textured water, jets, flames, stars |
| `0x60` | 4 | as `0x63`, on untextured models | canal, basin, sky spheres |
| `0x28` | 2 | one float: texture scroll per frame | stream 0.003, fountain jets -0.007 |
| `0x0a` | 4 | one float, 25..170 | a draw range, probably |
| `0x2e` | 4 | two floats: near, far | a fade range, probably |
| `0x15` | 7 | three floats | a size or box |
| `0x0d` | 1 | none | flames, night glow, pole star - **night only** |
| `0x1d` | 1 | none | the same set plus a few lights |
| `0x30` | 2 | one float | jets, lights, clouds |
| `0x11` | ? | | sun and moon only - celestial motion? |
| `0x4e 0x4f 0x45 0x50` | | | moon and sky only; `0x50` on every sky object |

**The model id is the four-character id of the MMB chunk, not its
sixteen-byte name.** `funm` is the chunk holding `funmiz`, `alls` holds
`allsea`, `lowc` holds `lowcol`. Ids are not unique within a DAT (`kabe`
names three chunks); the first match has been right so far.

Position, rotation and scale are in the DAT's frame and go through the same
transform as an MZB placement. Verified by eye on zero and ±π/2 rotations.

## The sky

The `weat/<weather>` generators place the sky **at the origin** with large
scales: the cloud dome `cld_fine_a01` at 18, the sun sphere at 70, the moon
at 20, the star field `star` forty units up at 3. The origin means "around
the camera": these move with the viewer. The cloud dome and star field are
textured (`fine    fine_a01`, `star    star01`); the sun and moon spheres
are not, and get their look from their texture animation (`k000`), which is
not yet read.

Which of `clod`, `fine`, `mist`, `suny` is used is the zone's current
weather, which the server sends (packet `0x057`). MogHouse reads only `fine`
so far.

### How this was found

The stars gave themselves away. When the effect pass first placed every
generator model with a texture animation, Bastok Markets' sky filled with a
flock of grey triangles. Listing which models had been placed
(`MOGHOUSE_LIST_GENERATORS=1 ffxi-datdump`) showed `star` and `moonsphere`
among them; the chunk tree put their generators under `weat/fine/star` and
`weat/fine/moon`; and `tools/generators.py ... weat` showed them all at
position 0, 0, 0 with scales of 3 to 70. A thing placed at the origin at
scale 70 that appears in the sky is the sky. Retail's night sky, in a
side-by-side, has exactly that cloud layer and star field.

The star sheet is white points on black, so drawn with ordinary alpha
blending the black drew too - the grey triangles. Added instead (source
plus destination) only the points show.

## The fountain, and what fire is

Bastok Markets' fountain, `effe/funs`, is a good small example of a whole
effect:

| generator | model | what |
|---|---|---|
| `fnmz` | `funm` = `funmiz` | the basin's water, untextured, rotated flat, scale 3.3 |
| `twa1..4`, `twb1..4` | `sibb`, `funt` | the jets: meshes textured `umi02`, animation `watm`, scroll -0.007 |
| `3ws1..3`, `3wy1..3` | `3wa1`, `3way` | the three-way spouts |
| `in01..08` | `sh-n` | ring of small splashes, animation `seat` |
| `wat1..7` | `awan` | foam (`awa` = bubble), op `0x07` - a particle emitter |
| `sfl1..4`, `bfl1..3` | `hi12` | **the flames**, night only (`0x0d`), animation `frtm` |
| `sll1..4`, `bll1..3`, `llit` | `lt` | the glow around each flame, night only |

`hi12` and `lt` are not in the zone DAT as models. `lt` is a texture chunk
there; `hi12` is nowhere in the file. A scan of every DAT under `ROM/`
(`scratchpad`-style script over `tools/datscan.py`) found `hi12` in
**`ROM/0/0.DAT`, the shared effects file**, as a type `0x1f` model with its
own `0x20` texture, `0x21` animation and `0x07` scheduler, and as empty
80-byte `0x2E` stubs in many zone DATs. So a flame is a shared, camera-facing
animated sprite, and the same `hi12` most likely lights every lamp and
lantern in the game. Reading type `0x1f` is the next step.

Retail confirmed at 20:35 in Bastok Markets: the flames and lamp fires are
lit, and the fountain's jets are **off**. No opcode found yet marks the jets
day-only; `0x0d` marks the flames night-only.

## What MogHouse draws today

- Water: every generator whose model is water (see [[Water]]).
- Effects: every `effe` generator whose model is textured and names a
  texture animation, as a mesh with its texture scrolled at the `0x28` rate,
  skipped between 06:00 and 18:00 when `0x0d` is set. The jets and the
  waterfalls come out of this.
- Sky: the textured `weat/fine` objects, camera-relative and unfogged;
  clouds lit by the zone's light and drifting, stars added and night-only.
  A zone with no `weat` directory (Sel Phiner, the sign-in backdrop) borrows
  West Ronfaure's, as it borrows its lighting.
- Not yet: particle emitters, the fish and birds (animated), the sun and
  moon, weather other than fine, the flames.
