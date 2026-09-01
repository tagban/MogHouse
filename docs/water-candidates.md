# Water models: what is drawn, and what might be missing

Water is placed geometry recognised by model name (`isWaterModel`, in
`renderer/scene.cpp`), not derived from the MZB's per-cell height. The height
field is read but deliberately not drawn - see the comment there: the values
track each cell's own floor at a constant offset, and the cells that looked like
the Bastok Markets fountain turn out to be concrete barriers in the real client.

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
