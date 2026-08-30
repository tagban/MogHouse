# MMB - FFXI models

Read from the retail files, not from anyone's source. Chunk type `0x2E`.

## Two-stage obfuscation

Both stages key off byte 5 of the payload.

**Stage one**, when byte 3 is 5 or more: every byte from offset 8 up to the
declared length is XORed with a rolling key.

```
key = key_table[payload[5] ^ 0xF0]
count = 0
for pos in 8 .. length:
    x = ((key & 0xFF) << 8) | (key & 0xFF)
    key += ++count
    payload[pos] ^= x >> (key & 7)
    key += ++count
```

The key advances **twice per byte**, and the shift uses the key *after* the
first advance. Getting that ordering wrong yields plausible-looking rubbish
rather than an obvious failure, so it is worth being exact.

**Stage two**, when bytes 6 and 7 are both `0xFF`: the body from offset 8 is
split in half and 8-byte blocks are exchanged between the halves wherever a
second key is odd, using `key_table2`.

Both tables come from the retail client and are not in this repository.

## Header

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | low 24 bits length, byte 3 version |
| 4 | 4 | key material - byte 5 seeds both stages |
| 8 | 8 | group name. `tshimono` on every model in East Sarutabaruta, so it looks like a set or zone identifier rather than anything per-model |
| 16 | 16 | **model name, space padded** |
| 32 | 4 | unidentified, observed 1 |
| 36 | 24 | bounding box: x min/max, y min/max, z min/max |
| 60 | 4 | unidentified, observed 0x40 |
| 64 | 4 | unidentified, observed 7 |

The name at offset 16 is what connects models to the world: **it matches the
16-character model id in an MZB placement entry.** That is the link that turns
4,918 placements into actual geometry.

Verified against East Sarutabaruta - 168 model chunks decrypt to readable names
including `lake_1_m`, `_sal_w01_h`, `_sal_w02_m`. Random bytes do not decrypt
into a zone's asset list.

## LOD

Names end in `_h`, `_m` or `_l` - high, medium and low detail. `_sal_w01_h`,
`_sal_w01_m` and `_sal_w01_l` are three versions of one object.

**The placement data picks which one.** Measured across East Sarutabaruta: of
4,918 placements, 4,321 name a `_m` model and 33 name a `_h` one, and of the 82
objects that have LOD variants at all, **none is placed at more than one**. So a
renderer draws what it is told and needs no distance policy for static geometry,
and the fear that a naive renderer would draw everything three times does not
arise - at least here.

Worth re-checking on a zone built for a later expansion before treating it as
universal.

## Asset reuse

Those 4,918 placements resolve to **98 distinct models** - about 50 uses each.
Repetition across a zone is how the game is built, not a bug in the reader.

## Mesh data

After the header at offset 60 comes either a single block, or - when that field
is non-zero - a table of offsets to several.

**Block header**, 32 bytes: a mesh count, the block's own bounding box as six
floats, and a face count.

**Mesh header**, 20 bytes:

| offset | type | field |
| --- | --- | --- |
| 0 | char[16] | texture name |
| 16 | u16 | vertex count |
| 18 | u16 | blending mode |

**Vertices**, either 36 or 48 bytes each. **Byte 4 of the chunk selects which**:
2 means the longer layout, which carries an extra displacement vector between
position and normal.

| | 36-byte | 48-byte |
| --- | --- | --- |
| position | 0 | 0 |
| displacement | - | 12 |
| normal | 12 | 24 |
| colour (u32) | 24 | 36 |
| uv | 28 | 40 |

**Indices** follow: a `u16` count in a 4-byte field, then that many `u16`, and
then **padding to a 4-byte boundary**.

That padding is easy to miss and expensive to miss. An odd index count leaves
the cursor two bytes short, so the next mesh header reads misaligned - its
texture name comes back with two leading nulls - and every mesh after it in that
model is rubbish. In East Sarutabaruta it silently degraded 1,238 of 4,918
placements while only 12 models reported an outright failure, because a parser
that stops at the first bad offset returns a partial model rather than an
error.

They are a **triangle strip** unless the chunk id begins `MMB` or the chunk uses
the 48-byte layout, in which case they are a plain list. Strips use degenerate
triangles to jump between pieces, and every other triangle is wound the opposite
way - both have to be handled or the model comes out with stray faces spanning
it.

Verified against East Sarutabaruta: 156 models, 167 meshes, 52,532 vertices,
35,811 triangles, with **zero vertices outside the bounding box each model
declares for itself** and zero indices out of range. That bounds check is the
one that matters - a wrong stride scatters vertices outside it immediately. Two
normals out of 52,532 are not unit length, which is likely genuinely degenerate
geometry rather than a parsing fault.

## Still to work out

- `hit_...` models are exactly 64 bytes: a header and nothing else. They are
  collision proxies with no geometry, so an empty model is the correct result
  rather than a parse failure.
- Texture names look like two 8-byte halves rather than one 16-byte string:
  `model   sar_kk2` reads as `model` and `sar_kk2` padded separately.
- How a texture name reaches the DXT3 chunk that holds the image
- What the group name at offset 8 selects
- The fields at 32 and 64
