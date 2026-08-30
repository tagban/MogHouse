# OS2 — skinned meshes

Chunk type `0x2A`. The geometry of one equipment slot, one NPC part, or one
weapon. Read by `renderer/ffxi/os2.cpp`.

**Every offset and size in the header counts 16-bit words, not bytes.** Reading
them as byte offsets lands in the middle of the draw stream and looks almost
plausible, which is worse than failing outright.

## Header, 64 bytes, packed to two

| offset | size | meaning |
| --- | --- | --- |
| 0 | 2 | unidentified |
| 2 | 2 | flags: bit 7 selects the bone table, the low bits gate normals |
| 4 | 2 | mirror; non-zero means the mesh is half a body |
| 6 | 4 | draw stream offset |
| 10 | 2 | draw stream size |
| 12 | 4 | bone reference table offset |
| 16 | 2 | bone reference count |
| 18 | 4 | weighted vertex counts offset |
| 22 | 2 | maximum weights per vertex |
| 24 | 4 | weight data offset |
| 28 | 2 | weight data count |
| 30 | 4 | vertex data offset |
| 34 | 2 | vertex data size |
| 36 onwards | | unidentified offsets and counts |

## The draw stream

A small command language. Every command carries its own length, so an
unrecognised one has to stop the walk rather than desynchronise everything
after it.

| command | payload |
| --- | --- |
| `0x8010` | draw state, 44 bytes. Specular exponent at +36, intensity at +40 |
| `0x8000` | material: a 16-byte texture name |
| `0x0054` | triangle list: `uint16` count, then count × 30 bytes |
| `0x5453` | triangle strip: `uint16` count, one 30-byte triangle then count−1 × 10 |
| `0x4353` | 8 bytes plus `uint16` count × 2 — skipped |
| `0x0043` | `uint16` count × 10 — skipped |
| `0xFFFF` | end |

A triangle is three `uint16` vertex indices followed by three `vec2` UVs; a
strip continuation is one index and one UV.

**UVs live in the draw stream, not on the vertex.** One vertex legitimately
appears with several different UVs, so corners cannot be shared and the
geometry has to be expanded per corner.

## Vertices

Two runs, whose lengths are the two `uint16` at the weighted vertex counts
offset: first the vertices with one influence, then those with two.

- one influence: `vec3` position, `vec3` normal — 24 bytes
- two influences: six floats of position **interleaved component by
  component** (x₀ x₁ y₀ y₁ z₀ z₁), two weights, then six of normal the same
  way — 56 bytes

Bone references sit in their own block, two 16-bit words per vertex, each
packing `bone : 7`, `mirror bone : 7`, `mirror axis : 2`. The one-influence run
consumes them in pairs too and uses only the first of each. When flag bit 7 is
set the seven-bit values index the bone reference table rather than the
skeleton directly.

**The position stored against each influence already carries that influence's
weight.** Skinning is therefore

    position = Σ Rᵢ · pᵢ + tᵢ · wᵢ

with the bone's accumulated world rotation `R` and translation `t`. Folding the
translation into a single matrix and weighting the whole thing weights it twice
for one influence and not at all for the other, which pulls every two-bone
vertex — shoulders, hips, knees — off the joint.

## Mirroring

Almost every character mesh is half a body. The second half comes from a second
pass over the same triangles using each influence's mirror bone, with the
position and normal reflected through the mirror axis in the bone's own space
before the bone rotates them. An axis of 0 means the vertex sits on the centre
line and is shared.

## Verified

`ROM/3/6.DAT`, a complete hume male NPC — seven meshes, 227 vertices in the
largest, zero corner indices, bone indices or weights out of range. Across the
five characters checked, weights sum to 1.0 on every vertex, and the resulting
bodies are symmetric about the mirror axis to the float: z runs −0.423 to 0.423.
