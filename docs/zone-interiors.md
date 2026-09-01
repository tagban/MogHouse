# Building interiors are separate DAT files

A city zone's DAT does not contain the insides of its buildings. Windurst
Waters is file 338, and everything in it is drawn - 1683 placements of 303
models, all resolved, all textured - and the result is still an empty shell
where a retail client shows a furnished room.

The furnishings are in their own files, each a self-contained scene with its
own MZB, in local coordinates near the origin:

| file | ROM path | MZB id | placements | what it is |
|---|---|---|---|---|
| 525 | ROM/2/61 | `b1`   | 79 | basement |
| 526 | ROM/2/62 | `1f`   | 64 | first floor |
| 527 | ROM/2/63 | `2f`   | 27 | second floor |
| 528 | ROM/2/64 | `miko` | 60 | shrine |

`1f` holds exactly what a Windurst shop interior looks like: `wi505_wall`
(8 meshes, 1550 triangles), `wi505_tukue` the counter, `wi505_lanpu` the
hanging lamp, `wi505_flag` the banners, plus notice boards, books, papers, a
pen, a chest of drawers and eight `komono` knick-knacks. Bastok's equivalent is
file 384 (ROM/1/71): `ba211_gawa` walls, `yukaston` floor stone, a chair, a
stove.

## How they line up with the zone

The interior files and the zone share models. Zone 238 places `_win_m01_h` and
`_win_m01_t` in pairs - (-60.00, -5.00, 90.00) beside (-54.25, -5.00, 90.00) -
and `1f` places the same two models in its own pairs at x = +-8.00 and
+-5.52. Their texture, `win_m01c`, is cobblestone, grass and leaves, so these
are entrance pieces rather than room geometry: the same doorway exists on both
sides of the boundary, which is what an interior can be aligned by.

The exact alignment is not worked out yet, and neither is what tells the client
to load `1f` rather than `2f`.

## The rule we had is wrong

Model file = `100 + zone` holds for the early zones and nothing else. Files
525-528 would be zones 425-428, and 425 and 428 do not exist at all. A zone's
DAT set is a table in the client, not arithmetic, and we have been loading one
file per zone on an assumption that quietly stops being true.

## How this was found, and what it cost

Four rounds of looking in the wrong place, each of which produced a clean
result that meant nothing:

- The unparsed MZB regions. They are a bounding-box tree, an index pool and 256
  `LTnn` light records - real finds, but not geometry.
- Models the zone never places. All 47 are the low-detail twin of a placed
  model, `wi_k_in0eal` beside `wi_k_in0eah`.
- Dropped or empty models. None: 357 parse, all carry triangles, and
  `models.emplace` loses only five duplicate sky models that nothing places.
- Texture lookups. All resolve.

What broke the loop was decoding a texture and looking at it. `win_m01c` is
cobblestone and grass, which said the models around the player were outdoor
pieces drawn correctly, and that the missing room was not in that file at all.
Reading the file we had could never have shown that; it took comparing against
a retail client standing in the same spot.

Related: `tools/dumptex.py`, `tools/nearby.py`, `MOGHOUSE_MZB_DUMP`.
