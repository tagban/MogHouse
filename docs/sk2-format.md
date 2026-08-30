# SK2 — skeletons

Chunk type `0x29`. One per character. Read by `renderer/ffxi/skeleton.cpp`.

Everything is packed to two bytes, so no field sits on its natural alignment
and each one has to be read by offset.

## Layout

| offset | size | meaning |
| --- | --- | --- |
| 0 | 2 | unidentified, zero in everything measured |
| 2 | 2 | bone count |
| 4 | 30 × bones | the bones |
| after | 2 | attachment point count |
| +2 | 2 | unidentified |
| +4 | 26 × points | the attachment points |

### Bone, 30 bytes

| offset | size | meaning |
| --- | --- | --- |
| 0 | 1 | parent index; the root points at itself |
| 1 | 1 | flag, 0 or 1 |
| 2 | 16 | rotation quaternion, x y z w |
| 18 | 12 | translation, relative to the parent |

**The parent is one byte.** It is usually written down as a `uint16`, which
folds the flag byte into the index: on the hume male that sends 50 of 94 bones
out of range, with values like 258 and 345 where 2 and 89 were meant. The giveaway
that the stride is nonetheless right is that every quaternion reads as unit
length.

The flag byte splits the hume male 44 to 50. What it selects is not known.

### Attachment point, 26 bytes

| offset | size | meaning |
| --- | --- | --- |
| 0 | 2 | bone index |
| 2 | 12 | three unidentified floats |
| 14 | 12 | offset from the bone |

These are the slots other things hang from — weapon trails, spell effects. The
index in the list is the slot number the rest of the game refers to.

## Where the chunk ends

The hume male skeleton chunk is 7,424 bytes and the bones and points account
for 6,772 of them. The rest is `cd cd cd cd` — uninitialised heap fill from
whatever wrote the file — which is a reliable marker for where the real content
stops, and is what settled the attachment point count at 128 rather than the
120 it is usually given as.

## Verified

Every playable race, read through `ffxi-datdump`:

| file | chunk | race | bones | attachment points |
| --- | --- | --- | --- | --- |
| 7072 | `hum_` | hume male | 94 | 128 |
| 10248 | `huf_` | hume female | 97 | 128 |
| 13424 | `elv_` | elvaan male | 99 | 128 |
| 16600 | `elv_` | elvaan female | 99 | 128 |
| 19776 | `tar ` | tarutaru | 93 | 128 |
| 23176 | `mit ` | mithra | 108 | 128 |
| 26352 | `gal ` | galka | 107 | 128 |

The two elvaan share a chunk id and a bone count but are different files. The
tarutaru file serves both sexes, which is why `tools/pcmodels.py` derives the
same base for each.

Every skeleton is a tree: exactly one self-parent, no cycles, every parent in
range.
