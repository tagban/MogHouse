# Effect generators: how a zone places its water

Read from the retail files on 2026-09-03, checked against East Ronfaure
(`ROM/0/121.DAT`) and Bastok Markets (`ROM/1/35.DAT`), and against a
LandSandBoat server: a character standing in East Ronfaure's stream at server
position 262, 44 reports height -39, and the generator that places the stream
piece under that spot puts its surface at -38.

## The problem it answers

Every zone's DAT holds water models that the MZB's placement table never
mentions. East Ronfaure has thirty-six of them, `ka1`..`ka22` and
`kb1`..`kb14`, each a hand-built surface a few units wide with its own slope;
Bastok Markets has `mizu` (the canal), `funmiz` (the fountain basin) and
`allsea` (the sea beyond the harbour wall). `docs/mzb-format.md` used to say
"the models exist in the DAT and nothing references them". Something does.

## Where they are placed

Each DAT is a tree of chunks (`docs/mzb-format.md` has the container). The
terrain models sit under `mode`; the water models sit under `effe`, in a
sub-directory per effect - `kawa` (river) in East Ronfaure, `sea`, `seaa` and
`funs` (fountain) in Bastok Markets. Alongside them, in the same directory,
are **type 0x05 chunks: effect generators**, one per placed thing.

FFXI's resource type table calls 0x05 *Generator* and 0x07 *Scheduler*. The
generators place the water, the fish (`sakn` holds `gyi`/`gyj`, the schools),
the pigeons (`hato`), the torch flames and the fountain jets; the schedulers
run their timing. Only the water is placed by MogHouse so far.

## The chunk

Offsets are from the start of the chunk, its sixteen-byte header included.
The header's own four-character id is the generator's name (`ka01`), and is
not the model's.

| offset | field |
|---|---|
| 0x40 | three floats, all 1.0 |
| 0x50 | four floats, all 1.0 |
| 0x80 | four `u32`: the offsets of four opcode streams |
| 0x90 | the first stream begins here in every generator seen |

Each stream is a sequence of opcodes and ends at an op `0x00`:

```
u8  op
u8  length, in four-byte words, header included
u16 zero
... payload
```

The opcodes that place a model:

| op | words | payload |
|---|---|---|
| 0x01 | 12 | +4 flags, **+8 model id (four chars)**, **+16 position x y z** (floats) |
| 0x09 | 4 | rotation x y z, radians |
| 0x0f | 4 | scale x y z |
| 0x63 | 4 | +4 texture animation id (four chars): `tkwa` on every river piece, `tkta` on the waterfalls |
| 0x0a | 4 | one float, 170 or 50 or 130 - a range or a lifetime, not yet known |

The **model id is the four-character id of the MMB chunk**, not its
sixteen-byte name: the fountain basin's generator says `funm` and the chunk
`funm` holds the model named `funmiz`; `alls` is `allsea`, `lowc` is
`lowcol`. Chunk ids are not unique in a DAT - `kabe` names three chunks,
`kabeana01..03` - and the first match is taken; nothing seen so far minded.

Position, rotation and scale are in the same frame and conventions as an MZB
placement and go through the same transform (`mh::placementTransform`).
Checked by eye in both zones: the stream sits in its channel, the canal in
its cut, the basin in its fountain. Rotations other than zero and ±π/2 about
y have not been checked.

## What it means for the water

The water models are ordinary meshes, either textured with a ripple sheet
(`effect  kaw1` on the stream, `sea     sea01` on the harbour) and flagged
for blending, or untextured (`mizu`, `funmiz`, `water`, `water2`) with the
sheet painted on at run time. Either way they carry their own heights - a
stream slopes, a basin sits above the plaza - and are tucked under the banks
so the depth test clips them at the shoreline. That is why retail's water
hugs every rock: the geometry was modelled to, and the bank hides the rest.

MogHouse now:

- reads every generator (`renderer/ffxi/generator.cpp`), resolves the model
  by chunk id, and adds a placement for every one whose model is water
  (`mh::isWaterMesh`: a water name, or a mesh textured with one of the ripple
  sheets);
- routes water meshes to the water pass as world-space triangles with the
  mesh's own UVs (`renderer/scene.cpp`), so the ripple runs the way the
  stream does;
- falls back to the collision-derived sheets (`tools/makewater.py`) only in
  a zone with no water mesh at all.

`MOGHOUSE_ALL_GENERATORS=1` places every generator's model, water or not,
which is how to see where the fish and flames go before there is anything
to animate them with. `MOGHOUSE_LIST_GENERATORS=1 ffxi-datdump <zone.DAT>`
lists them; `python tools/generators.py <zone.DAT> [directory]` does the same
without a build.

## Not yet read

- The time-of-day scheduling. Bastok Markets' fountain runs jets by day and
  flames by night; whatever switches them is in these chunks or in the `0x07`
  schedulers beside them.
- Everything a generator says beyond placement: the particle behaviour that
  makes the fountain's `funtw1` a spray rather than a static sheet of `umi02`.
- Op `0x60`, which the untextured water uses where the textured water uses
  `0x63`.
- What the four opcode streams are for individually; the second holds the
  placement in every generator seen.
