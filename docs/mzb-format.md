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
| 4 | 4 | low 24 bits: number of placement entries |
| 8 | 24 | not yet identified |
| 32 | n * 0x64 | placement entries |

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

## Still to work out

- The 24 bytes between the counts and the placement table
- The collision mesh table: vertices, normals, indices and their bounds
- The quadtree used for visibility
- What the four floats at offset 52 select
