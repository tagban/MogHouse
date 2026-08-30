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
`_sal_w01_m` and `_sal_w01_l` are three versions of one object, so a renderer
gets to choose, and a naive one that draws all three will draw everything three
times.

## Still to work out

- The mesh data itself: vertices, indices, and how they are grouped
- Material and texture references, and how they reach the DXT3 chunks
- What the group name at offset 8 selects
- The fields at 32, 60 and 64
