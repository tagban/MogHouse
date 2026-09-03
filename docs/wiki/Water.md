# Water: how the retail client draws it, and how MogHouse matches it

Worked out 2026-09-03 against East Ronfaure and Bastok Markets, with a
LandSandBoat server for ground truth and a retail client side by side.
Everything below was read from the files; nothing is taken from another
project's source.

## The short version

**Water is ordinary geometry in the zone DAT**, hand-built by the artists,
placed by the **effect generator chunks** (see [[Effect-Generators]]) and
textured with a scrolling ripple sheet. It is not derived from terrain
heights, not a flag on the collision mesh, and not the MZB's per-cell "water
height" field. Every earlier attempt in this project tried one of those
derivations and every one produced the same family of faults: surfaces at the
wrong height, missing under bridges, stepped or triangular at the banks.

## What the artists built

East Ronfaure's stream is thirty-six meshes, `ka1`..`ka22` and `kb1`..`kb14`.
Each one is a small surface, a few units wide and up to forty long, with its
own vertex heights: the stream slopes, so each piece sits a little lower than
the one upstream (the generators place them from -48 at the south end to -7
at the north, in the DAT's y-down frame). Each is modelled slightly wider than
its channel and tucked **under the banks**, so the depth test clips it at the
shoreline. That is why retail's water hugs every rock: the mesh was made to,
and the bank hides the rest.

They are textured with `effect  kaw1`, a white sheet whose ripple lives in
its alpha, and carry the mesh blend flag `0x8000`. Bastok Markets' harbour
sea (`allsea`) uses `sea     sea01` the same way. Its canal (`mizu`) and
fountain basin (`funmiz`) name **no texture at all**; the client paints the
sheet on at run time, which is what generator opcode `0x60` most likely
selects (the textured pieces use `0x63` and name a texture animation such as
`tkwa`).

The MZB placement table never mentions any of these. They are placed by the
generators in the `effe` directory of the DAT, one generator per surface, on
a forty-unit grid for the stream.

## How this was found

1. The user asked how the retail client places water. The answer had to be
   in the DAT, because the server sends nothing about water.
2. `ffxi-datdump` with `MOGHOUSE_MODEL_STATS=1` listed every model in the
   zone with its texture. In East Ronfaure, 36 models named `ka*`/`kb*`
   shared the texture `kaw1`; in Bastok Markets, `mizu`, `funmiz`, `water`,
   `water2` had empty textures.
3. `MOGHOUSE_LIST_PLACEMENTS=1` showed the MZB table placed none of them,
   and their MMB header bounds were small and local, so they needed placing.
4. `tools/datscan.py`-based chunk-tree dumps showed the models under an
   `effe` directory beside 36 type `0x05` chunks in a sub-directory `kawa`
   (river). Searching every non-model chunk's body for the model ids found
   the generator that named each one.
5. Hex-dumping a generator with a float view showed the model id, three
   floats that were plausible world coordinates, and further blocks that
   were a rotation and a scale, each introduced by a byte opcode and a
   length in words. The layout is in [[Effect-Generators]].
6. Verified against the server: a character standing in the stream at
   server position 262, 44 reports height -39; the generator places piece
   `kb10` at 260, 60 with its surface at -38. Then by eye in both zones.

## What MogHouse does with it

- `renderer/ffxi/generator.cpp` parses every generator in the DAT.
- `loadZone` (`renderer/viewer.cpp`) resolves each generator's model by the
  MMB chunk's four-character id and adds a placement for every model that is
  water. A mesh is water when its model has a water name (`water`, `mizu`,
  `funmiz`, `sea*`, `suimen`...) or its texture is one of the ripple sheets
  (`kaw1`, `umi1`, `umi2`, `sea01`, `ike1`, `ike2`, `umna`, `nami`, `miz1`,
  `miz2`) - `mh::isWaterMesh` in `renderer/scene.cpp`. The blend flag is
  deliberately not a criterion: every stream piece has it.
- Water meshes go to the water pass as world-space triangles with their own
  UVs, so the ripple runs the way the stream does. The pass scrolls the sheet
  the zone's water names most, voted by triangle; untextured water votes for
  the river look, so Bastok's canal is not tinted like its harbour.
- The collision-derived sheets (`tools/makewater.py`, from the server's
  `.ximesh` files) are loaded **only** when a zone has no water mesh. They
  are the previous approach and remain a fallback.

## Tools

- `python tools/generators.py <zone.DAT> [directory]` lists a zone's
  generators: model, position, rotation, scale, texture animation.
- `MOGHOUSE_LIST_GENERATORS=1 ffxi-datdump <zone.DAT>` does the same and
  says whether each model resolved.
- `MOGHOUSE_ALL_GENERATORS=1` when running the client places every
  generator's model, water or not - the fish, birds and flames appear as
  still meshes where they belong.
