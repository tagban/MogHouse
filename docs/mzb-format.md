# MZB - FFXI zone layout

Written from reading the bytes of the retail DATs, not from anyone's source.
Every field below was verified against real files with `tools/datscan.py` and
`tools/mzbdecrypt.py`. Where a value is still a guess it says so.

## Finding it

A `.DAT` is a flat sequence of 16-byte-aligned chunks, which form a tree:

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | four-character id, e.g. `r_3b` |
| 4 | 4 | packed: `type:7`, `next:19`, `shadow:1`, `extracted:1`, `ver:3`, `virtual:1` |
| 8 | 4 | parent |
| 12 | 4 | child |

`next * 16` is the chunk's total length including the header. Type `0x00` closes
the current directory and `0x01` opens one. **MZB is type `0x1C`**; MMB is
`0x2E` and SK2 is `0x29`.

Verified across the retail `ROM/1` set: every zone DAT holds exactly one MZB,
between roughly 750KB and 4.7MB.

## The payload is encrypted

Byte 3 of the payload is a version. **`>= 0x1B` means encrypted**, and every
retail zone checked is.

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | low 24 bits: encrypted length. Byte 3 is the version. |
| 4 | 4 | low 24 bits: number of placement entries, top 8 flags |
| 8 | 4 | collision mesh table offset. **0 means the zone has none.** |
| 12 | 4 | grid width, grid height, bucket width, bucket height - one byte each |
| 16 | 4 | quadtree offset |
| 20 | 4 | object end offset |
| 24 | 4 | short names offset |
| 28 | 4 | unidentified, signed |
| 32 | n * 0x64 | placement entries |

Note offset 0 is *not* a four-character id, despite lotus's struct declaring it
`char id[4]`. The decryption reads the length from those same bytes, so it
cannot also be a name.

Decryption walks forward from offset 8 in variable-sized runs:

```
key = key_table[payload[7] ^ 0xFF]
counter = 0
pos = 8
while pos < length:
    run = ((key >> 4) & 7) + 16
    if (key & 1) and pos + run < length:
        xor the next `run` bytes with 0xFF
    key += ++counter
    pos += run
```

So only runs whose key happens to be odd are actually obscured, and the run
length varies between 16 and 23 bytes. After that pass, each placement entry's
16-byte model id is separately XORed with `0x55`.

`key_table` is 256 bytes taken from the retail client. **It is not in this
repository** - same position as `compress.dat`. The tools take a path to it.

## Placement entry - 0x64 bytes

| offset | type | field |
| --- | --- | --- |
| 0 | char[16] | model id, XORed with 0x55 |
| 16 | float[3] | translate x, y, z |
| 28 | float[3] | rotate x, y, z, in radians |
| 40 | float[3] | scale x, y, z |
| 52 | float[4] | unidentified; observed 0, 10, 100, 1000 |
| 68 | uint32[8] | unidentified |

Verified by decrypting `ROM/1/100.DAT` (zone `r_3b`, 45 placements) and getting
readable asset names with plausible transforms:

```
07_kibako01    59.32  -4.77  31.93   rot 3.14 5.39 3.14   scale 1 1 1
07_tubo        45.74  -4.83  30.03   rot 0.00 0.79 0.00   scale 1 1 1
t_atari7       52.74  -4.10  32.36   rot 3.14 5.50 3.14   scale 1 1 1
```

Random bytes do not decrypt into Japanese romaji - `kibako` is a crate, `tubo` a
pot, `tubodai` its stand. That is the check that the scheme above is right.

**`t_atari*` is collision geometry** - *atari* is a hit or contact. Worth knowing
before rendering a zone and wondering what the invisible walls are.

## Collision meshes

The table at `collisionMeshOffset` starts with a `u32` count and a `u32` offset
to the first entry. Entries are 16 bytes and run back to back - the next begins
at `triangle offset + count * 8`.

| offset | type | field |
| --- | --- | --- |
| 0x00 | u32 | vertex data offset |
| 0x04 | u32 | normal data offset |
| 0x08 | u32 | triangle data offset |
| 0x0C | u16 | triangle count |
| 0x0E | u16 | flags |

Vertices run as `vec3` from their offset up to the normal offset, and normals
likewise up to the triangle offset - the sizes are implied by the next offset
rather than stored. Triangles are four `u16` each: three indices masked to
`0x3FFF`, so the top two bits of each carry something not yet identified, plus a
fourth value.

Verified across the whole of `ROM/1` - **128 zones, 1,821,257 vertices,
2,105,385 triangles, zero indices out of range and zero non-unit normals**. Those
last two are the checks worth caring about: 2.1 million indices all landing
inside their own mesh, and every normal unit length, do not happen by accident
if an offset or stride is wrong.

Versions observed: `0x1B` on 127 zones and `0x1A` on one. The `0x1A` file is not
encrypted and parses with the same code, which is what the version check
predicts.

## Still to work out

- The quadtree at `quadtreeOffset`, used for visibility
- What the short names table holds
- The top two bits of each triangle index
- The fourth `u16` in each triangle
- Why normal counts do not match either vertex or triangle counts - 2,354
  normals against 3,248 vertices and 3,578 triangles in `r_3b`, so they are
  neither per-vertex nor per-triangle
- What the four floats at offset 52 of a placement select
- The `0x1C`-typed chunks appear twice in some DATs (`24.DAT` has two zones)

## Water

**Corrected 2026-09-03.** Water is not placed by the placement table, but the
models are referenced: by the **effect generator chunks** (type `0x05`) in the
`effe` directory, one per surface, each naming a model by its chunk id and
giving position, rotation and scale. See `docs/generator-format.md`. The
per-cell height below is real but is not what draws the water; it is kept
because `Collision::waterDepthAt` reads it.

The field: a **height carried on each collision grid entry**, 164 bytes into
the entry's placement block, as fixed point:

```
height = ((packed << 6) >> 10) / 1024.0
```

The shifts drop the top six and bottom four bits, which carry something else.
Zero means no water over that cell.

East Sarutabaruta has water on 9,291 of 49,128 cells at heights -27 to 25;
Bastok Markets on 4,664 of 10,844 at -4 to 8. So it is a large part of a zone
rather than an occasional feature.

The surface is a flat plane at that height covering the cell, **not** the
collision mesh translated upward - lotus leaves a TODO saying exactly that. The
renderer builds one quad per cell spanning where that cell's geometry reaches.

Heights are discrete - East Sarutabaruta has 13 distinct values across 9,291
cells, clean integers like -7, 1, 2, 25 - one per body of water. So a step
between neighbouring cells is a real boundary between separate bodies, not a
rounding artefact.

Beware computing the height in Python: `(packed << 6) >> 10` relies on the left
shift discarding the top bits at 32 bits, which Python's arbitrary-precision
integers do not do. Without masking to 32 bits and sign-extending by hand it
produces plausible-looking values in the thousands.

**Water is two layers.** The effect textures are pure white - `effect kaw1` and
`effect ike1` both average RGB (255,255,255) - so they are a foam and ripple
sheet rather than a water colour. Used as the base they read as wet concrete.
The body colour is `effect umna` at (12,15,29), a dark blue. So the surface is a
dark base with a scrolling white highlight over it.

**No single water texture is stored** - lotus generates one procedurally, and
so does this renderer. That means the *appearance* of water is invented: where
it is and how high it sits come from the file, but the colour, wave pattern,
fresnel and specular are all made up. Matching the retail look would be an
exercise in comparing against screenshots, not in reading the format.
