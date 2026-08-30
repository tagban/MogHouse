# Textures

Chunk type `0x20`. Unlike MZB and MMB these are **not obfuscated** - no key
tables needed.

## Header

| offset | type | field |
| --- | --- | --- |
| 0x00 | u8 | encoding flag |
| 0x01 | char[16] | name, space padded |
| 0x11 | u32 | unidentified |
| 0x15 | i32 | width |
| 0x19 | i32 | height |
| 0x1D | u32[6] | unidentified |
| 0x35 | u32 | bytes per row |
| 0x39 | char[4] | `3TXD` for the block-compressed encoding |
| 0x3D | u32 | payload size |
| 0x41 | u32 | block count |
| 0x45 | | pixel data |

The name is in the same 16-byte space-padded form a mesh header uses for its
texture, so the two match directly: a mesh asking for `model   sar_kk2` wants
the chunk whose name is `model   sar_kk2`.

## Encodings

**`0xA1`** - DXT3 blocks, which is **BC2**, a format GPUs read natively. It goes
to the GPU untouched, no decoding.

**`0xB1`** - 8-bit indices into a 256-entry BGRA palette at 0x39, with rows
stored bottom-up. Expanded to RGBA8 on load.

`0x01` and `0x81` also exist in the wild but are not handled - nothing in the
zones read so far uses them.

## Verified

East Sarutabaruta holds 51 textures: 44 BC2, 7 paletted, none failing. BC2
stores one byte per pixel, so payload size should equal width times height
exactly, and it does for all 44 - a cheap check that the header offsets are
right, since a wrong offset would put a plausible-looking but wrong size in that
field.

## Alpha is not 0..255

Measured across East Sarutabaruta's textures, 1.37 million alpha texels:

| alpha | share |
| --- | --- |
| 0/15 | 34.4% |
| 7/15 | 32.7% |
| 8/15 | 24.6% |
| 15/15 | **2.4%** |

Almost nothing is fully opaque. The mass sits at 7 and 8 out of 15 - about 0.5 -
and **that is what opaque means here**, the same 0..128 scaling the vertex
colours use, where lotus divides by 128 rather than 255.

The practical consequence: any alpha cutoff above about 0.4 throws away most of
the world. A 0.35 threshold discards 39.5% of all texels, including the entire
opaque population.

**And on most surfaces alpha is not a cutout at all.** Terrain textures are half
or more alpha-zero while their RGB is never black:

| texture | alpha-zero | blocks with near-black colour |
| --- | --- | --- |
| `sar_kk2` (flat ground) | 60.5% | 0.0% |
| `sar_w1` (rock) | 51.0% | 0.0% |
| `sal_w02c` | 24.5% | 9.7% |

Testing against alpha on those removes most of the ground and leaves a
checkerboard of holes. The colour is meaningful everywhere, so terrain is simply
drawn opaque and its alpha ignored.

Which surfaces want a cutout is **not** stated anywhere in the format. Two
candidates were tried and abandoned:

- **The mesh header's `blending` field** marks base against overlay within a
  tile, not transparency. The same texture appears under both flags - `sar_kk2`
  has 53 meshes at `0x0000` and 52 at `0x8000` - so it could never have
  separated foliage from ground.
- **Surface orientation.** Plausible, since foliage is billboards and ground is
  flat, but a grass tuft is crossed, splayed quads measuring 0.57 rather than 0,
  and no threshold separated it from ground without also cutting holes through
  cliff faces. Worth noting that grass billboards carry normals pointing
  straight up - so they light like the ground beneath - which makes stored
  normals useless here even before that.

What does work is **how transparent the texture is**, measured at load:

| texture | alpha-zero | treatment |
| --- | --- | --- |
| `sar_hg_0` (grass) | 0.95 | cutout |
| `sar_kaw1` (river) | 0.80 | opaque |
| `sar_kk2` (ground) | 0.60 | opaque |
| `sar_w1` (rock) | 0.51 | opaque |

The threshold sits at **0.88**, bracketed between the river and the grass. Each
neighbour was established by a visible failure: below 0.80 the river edges break
up, above 0.95 the grass renders as black boxes.

**This is fitted, not derived.** The margin is 0.15, tuned against one field
zone. A texture landing between 0.80 and 0.95 elsewhere is a coin toss, and this
is the first thing to look at when a town or later-expansion zone renders
wrongly.

## Still to work out

- The `0x01` and `0x81` encodings
- Mip levels: none of these carry any, which matters for the upscale packs -
  those will need mips generated rather than loaded
- The six unidentified words at 0x1D
