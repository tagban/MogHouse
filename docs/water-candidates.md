# Water: solved

**Water is a material on each collision triangle.** Not a model you can
recognise by name, and not the MZB's per-cell height field - both were tried
and both were wrong, and the rest of this file is the record of those attempts.

LandSandBoat keeps `material:4` and `barrier:1` per collision triangle
(`src/map/ximesh/ximesh_structs.h`), and its `TerrainType` puts ShallowWater at
8 and DeepWater at 9. That is what `!pos` prints as `Terrain: Deep Water`, and
what `zone:getTerrainType(pos)` answers. The decoded meshes ship with the
server as zlib-compressed `.ximesh` files, one per zone.

Read with `tools/ximesh.py <zone>`. Checked across four zones, and the numbers
are what a city should look like:

| zone | triangles | water | and the rest |
|---|---|---|---|
| Windurst Waters | 67,079 | **5,360 (8.0%)** | Wood 28.6%, Stone 24.8%, Grass 16.6% |
| Windurst Woods | 52,132 | 432 (0.8%) | Stone, Wood, Object, Grass |
| Bastok Markets | 40,998 | 697 (1.7%) | Stone 77%, Wood, Object, Metal |
| Southern San d'Oria | 55,762 | 32 (0.1%) | Stone 64%, Wood 27% |

Windurst Waters is far and away the wettest, which is what anyone who has stood
in it would tell you.

## What is left to do

The triangles are in each block's local space; the block's placement transform
(a 3x3 rotation and a translation, in the same file) still has to be applied
before they are world geometry. After that they are ordinary surfaces and can
be drawn the way the named water models already are.

## Why the earlier attempts failed

Kept because both are easy to have again, and the second one nearly shipped.

## What matches today

Across all 143 zones with a model DAT, 9,695 distinct placement models exist.
The current rule matches 13 names across 11 zones:

| model | placements |
|---|---|
| `sea1_m` | 4628 |
| `water2` | 292 |
| `water` | 100 |
| `water1` | 17 |
| `sea_grid` | 12 |
| `water0`, `water3`, `water4` | 1 each |

## Candidates worth a look

These are real placement names - read through the MZB parser, not scraped from
bytes - that read as water but are not matched. **None are verified.** Adding
one that is not water paints a translucent surface over solid ground, which is
the mistake the MZB height path already made, so each wants eyes on it first.

| model | placements | why |
|---|---|---|
| `kawa_1_m`, `kawa_2_m`, `kawa_3_m` | 57 | *kawa* is river, and the numbering is clean |
| `mizu002` | 6 | *mizu* is water |
| `pool03`, `pool05`, `pool06`, `tr_pool` | 65 | named in English |

## Traps

Substring matching on Japanese words is unsafe, and these are why the rule was
not simply widened:

- `takibi_01` - *takibi* is a **bonfire**. It contains *taki* (waterfall).
- `gu_tumi01_m`..`gu_tumi03_m` - *tumi* is a pile. Contains *umi* (sea).
- `tukawaku` - contains *kawa* (river) and means nothing of the sort.
- `umigake01_m`, `umisaku01` - *umi* plus cliff and *umi* plus fence: terrain
  **beside** the sea, not the sea.
- `hit_hashi_umi` - *hashi* is a bridge.

## How this list was made

`MOGHOUSE_LIST_PLACEMENTS=1 ffxi-datdump <zone DAT>` prints every placement as
`name x y z`. A first attempt scanned the DAT bytes for ASCII instead, and a
control run for the names already known to be water - `suimen`, `water`,
`lowsea` - found them in **zero** zones, which showed the scan was reading noise
rather than model names. Worth repeating that control on any similar search.
