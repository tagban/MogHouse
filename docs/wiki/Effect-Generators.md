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
are **intensity curves** named by the generators (`watm`, `frtm`, `tkwa`),
and type `0x21` chunks are texture animations.

## The day/night switch: type 0x19 intensity curves

A `0x19` chunk is sixteen bytes of header and then pairs of floats,
`(time, value)`, time being a fraction of the Vana'diel day from midnight.
Trailing `(0, 0)` pairs are padding: stop when time goes backwards. A
generator names its curve with op `0x63` (op `0x60` on an untextured model).
Read from Bastok Markets and East Ronfaure:

| curve | used by | keys | meaning |
|---|---|---|---|
| `frtm` | fountain flames | (0, .78) (.26, .78) (.28, 0) (.72, 0) (.74, .78) (1, .78) | lit 17:49 to 06:12 |
| `mrtm`, `fflt` | lamp glows | same shape | lit at night |
| `watm` | fountain jets | (0, 0) (.26, 0) (.28, 1) (.74, 1) (.76, 0) (1, 0) | **on by day only** |
| `ksta` | stars | (0, 1.34) (.13, 1.04) (.23, 0) (.73, 0) (.88, 1.14) (1, 1.34) | out 21:03 to 05:36, brightest at midnight |
| `k000` | moon | (0, 1) (.16, 1) (.30, 0) (.71, 0) (.83, 1) (1, 1) | out at night |
| `tkwa` | the stream | (0, .39) (.37, .58) (.62, .58) (1, .39) | always on, brighter by day |
| `tkta` | waterfalls | (0, .75) (.50, 1.4) (1, .75) | always on |
| `koa1` | wall-hole lights, East Ronfaure | (0, 1) (.20, 1) (.22, 0) (.75, 0) (.75, 1) (1, 1) | lit at night |

So the flame/jet switch the user saw in retail at 20:35 - lamps lit, jets
off - is these two curves. MogHouse evaluates the named curve at the current
clock and draws the thing while the value is above 0.05; the value itself
(a brightness) is not yet applied. Before the curves were read, op `0x0d`
was used as a night-only flag; it agrees with the curves on everything seen
but is not the mechanism.

How found: the curves were first taken for UV animations because the jets'
`watm` named one and its values were 0..1. Dumping `frtm` beside `watm`
showed them to be mirror images with steps at 0.26/0.28 and 0.72/0.74 of
something - hours 06:15/06:45 and 17:15/17:45 of a day. `ksta` (stars) and
`k000` (moon) then fitted the same reading.

## Op 0x27: the water table

Bastok Markets' `allsea` and `lowsea` generators, and only those, carry op
`0x27`. `allsea` is a 1,155-triangle sheet scaled a hundredfold to span the
whole zone at heights from -10 to +10, one level under every floor - a water
table, visible only where the ground is cut. Drawn directly it covered the
auction house floor with rippling water. MogHouse does not place a generator
that carries `0x27`; what the retail client does with these sheets (a
reflection pass, most likely) is not known.

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
| `0x63` | 4 | +4 **intensity curve** id (4 chars), a 0x19 chunk - see below | textured water, jets, flames, stars |
| `0x60` | 4 | as `0x63`, on untextured models | canal, basin, sky spheres |
| `0x28` | 2 | one float: texture scroll per frame | stream 0.003, fountain jets -0.007 |
| `0x0a` | 4 | one float, 25..170 | a draw range, probably |
| `0x2e` | 4 | two floats: near, far | a fade range, probably |
| `0x15` | 7 | three floats | a size or box |
| `0x0d` | 1 | none | flames, night glow, pole star - coincides with night-only curves |
| `0x1d` | 1 | none | the same set plus a few lights |
| `0x30` | 2 | one float | jets, lights, clouds |
| `0x11` | ? | | sun and moon only - celestial motion? |
| `0x4e 0x4f 0x45 0x50` | | | moon and sky only; `0x50` on every sky object |

**The model id is the four-character id of the MMB chunk, not its
sixteen-byte name.** `funm` is the chunk holding `funmiz`, `alls` holds
`allsea`, `lowc` holds `lowcol`. **Ids are not unique within a DAT**, and the
right one is the chunk in the generator's own directory. Bastok Markets has
two chunks called `auc_`: `auc_lt`, the lamp glow in `effe/ligh`, and
`auc_stdl`, the auction house stand with its stairs under `mode`. Taking the
first match placed the stairs a second time with a scrolling texture, and
they "flowed like water". `kabe` likewise names three chunks.

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
