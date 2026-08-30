# What makes a character

A player character in FFXI is a race and six model ids. Everything else — which
files to open, which bones to hang the geometry on, how it moves — follows from
those seven numbers.

    race, face, head, body, hands, legs, feet

`PORTJEUNO_LOOK=1,0,0,1,1,1,1` is a hume male with face 0 and the first
body, hands, legs and feet in the table, wearing no headgear.

## Where the files are

Each playable race owns one contiguous block of file ids. The skeleton sits at
the base and the slots follow at fixed offsets:

| offset | slot | entries | chunk id |
| --- | --- | --- | --- |
| +0 | skeleton and animations | 1 | `0x29` |
| +8 | the character's own head — face and hair | 32 | `hh_h` |
| +40 | headgear | 256 | `hh_m` |
| +296 | body | 256 | `hh_b` |
| +552 | hands | 256 | `hh_g` |
| +808 | legs | 256 | `hh_l` |
| +1064 | feet | 256 | `hh_f` |

A model id indexes straight into its slot's window: `file = base + offset + id`.
Ids with no file are gear that race never wears.

| race | base |
| --- | --- |
| hume male | 7072 |
| hume female | 10248 |
| elvaan male | 13424 |
| elvaan female | 16600 |
| tarutaru male | 19776 |
| tarutaru female | 19776 |
| mithra | 23176 |
| galka | 26352 |

Only the first block is resolved — model ids 0 to 255, the gear the game
shipped with. Later expansions added blocks elsewhere in the file table that
this does not yet reach.

### How that was derived

`tools/equipscan.py` indexes every DAT in the install holding a skinned mesh —
48,729 of them — classifying each by the race and slot its texture chunk ids
name. `tools/pcmodels.py` then scores every candidate base on how many of those
files land in the window the layout above predicts.

Score alone is not enough. Every expansion block repeats the same shape and
scores as well as the original, which is why an unqualified search picks 71207
for the hume male instead of 7072. The tie-breaker is that the race base is the
file holding that race's skeleton, and the expansion blocks hold no skeleton at
all. With that requirement all eight races resolve, and the eight numbers agree
with what the client is known to use.

Two things the scan settled that guessing would not:

- The race prefixes on texture chunk ids are `hm hf em ef tr tl mt gl`.
  Tarutaru are `tr` and `tl`, nothing like their initials.
- `hh_h` is the character's own head — face and hair — while `hh_m` is
  headgear. Reading `h` as "hat" puts the face in the helmet slot.

## Putting one together

1. Read the skeleton (`0x29`) from the race's base file — see
   [sk2-format.md](sk2-format.md).
2. Read every skinned mesh (`0x2A`) from the base file and each slot file — see
   [os2-format.md](os2-format.md).
3. Read the animations (`0x2B`) — see [mo2-format.md](mo2-format.md).
4. Pose the skeleton, skin the meshes onto it, and group the result by texture.

Skinning happens on the CPU. A zone is hundreds of thousands of triangles and
has to be instanced on the GPU; a character is around two thousand, and there
will rarely be more than a few dozen on screen. Doing it here means the result
drops straight into the pipeline the zone already uses.

## What comes out

Rendered in East Sarutabaruta, each in gear of its own:

| race | look | height | bones | triangles |
| --- | --- | --- | --- | --- |
| hume male | `1,0,0,1,1,1,1` | 1.79 | 94 | 2,016 |
| galka | `8,0,0,5,5,5,5` | 2.32 | 107 | 2,078 |
| tarutaru | `5,0,0,9,9,9,9` | 0.99 | 93 | 2,062 |
| mithra | `7,0,0,3,3,3,3` | 1.69 | 108 | 2,140 |

Those heights match the game, which is the check that matters: nothing in the
files states them, so they can only come out right if the bone chain, the
skinning and the mirroring are all correct together.

## Driving it

| variable | meaning |
| --- | --- |
| `PORTJEUNO_LOOK` | `race,face,head,body,hands,legs,feet` |
| `PORTJEUNO_CHARACTER` | semicolon-separated DAT paths, for an NPC that lives in one file |
| `PORTJEUNO_CHARACTER_AT` | `x,y,z` to stand it at |
| `PORTJEUNO_CHARACTER_FACING` | heading in degrees; the model faces east at zero |
| `PORTJEUNO_ANIMATION` | animation name, `idl0` by default |
| `PORTJEUNO_FRAME` | pins the animation clock, for a repeatable screenshot |

`c` in the window stands the character wherever the camera is.

## Not done yet

- Expansion equipment blocks, so model ids above 255 resolve.
- Weapons, which have their own slots in the same layout.
- The item table, which is what maps an item to a model id.
- Blending between animations, so a walk does not snap into an idle.
- Standing on the ground: nothing yet asks the collision mesh how high it is.
- A blended pass. Some headgear carries a dark eye slot that reads as a flat
  black bar. Both cutout modes render it identically and no texture is missing,
  so it is genuinely opaque in the sheet - but the retail client may blend it,
  which characters currently have no path for.
