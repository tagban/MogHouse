# Object mapping

How the server names a thing, and where its model actually lives.

Everything here is either read out of the retail files, confirmed against
LandSandBoat's source, or credited to someone who knew it already. Where two
sources disagree, both are given, because which one is right depends on whether
you are asking what the wire carries or what the client does with it.

## The look byte

Every entity's appearance arrives as a `look_t`, and its first field decides how
to read the rest. Ashita and Windower call this the **size** byte. That name is
misleading:

> only actual player objects use it as size in client, for everything else size
> is handled by flags and that byte is an index instead. SE just happened to use
> a separate index for multiple types of models.
> — LandSandBoat

So it selects **which model table an id belongs to**. The same number means a
different model under a different value, which is why one lookup rule cannot
serve all of them.

| value | LSB name | what it carries | what the client makes of it |
|---|---|---|---|
| 0 | `standard` | one model id | non-visible static object |
| 1 | `equipped` | race, face, 8 equipment ids | player type |
| 2 | `door` | no model - a name | doors |
| 3 | `elevator` | no model - a name, plus a timestamp | elevator or moving platform |
| 4 | `ship` | no model - a name, plus a timestamp | movable object |
| 5 | `misc` | one model id, its own table | a "binary name" in LSB; really an index code, and LSB's packet handling of it is wrong |
| 6 | `automaton` | one model id, its own table | used by besieged and campaign monsters in an "npc" state |
| 7 | `chocobo` | race and equipment, small indices | used in cutscenes; these are named like `NPC[FE]` in the DATs |

Two notes worth having:

- **1 is how you dress a monster.** Giving a mob an equipped look is how a Fomor
  gets visible gear.
- **5 and 6 are not what their LSB names suggest.** `misc` is used by Fomors;
  `automaton` covers trolls, lamia and mamool ja. LSB's own schema says so.

On the wire, LSB decides what arrives: `toLookFields` in
`src/map/data/shared_types/look.h` writes 7 in the first field for a chocobo and
nine equipment-shaped fields after it, so against an LSB server 7 reads as
equipment regardless of what retail would do with it.

## Where the fields sit

In the NPC entity update (server packet `0x0E`):

| offset | field |
|---|---|
| `0x25` | battle flags |
| `0x28` | render flags |
| `0x29` | allegiance |
| `0x2B` | name visibility |
| `0x30` | `look_t` |
| `0x34` | name - **or**, for a door, its model name |

The player update (`0x0D`) is a different layout: the look is at `0x48` and the
name at `0x5A`. Reading one with the other's offsets does not fail, it just
produces nonsense.

### Doors carry a name where the name goes

A door has no model id. `entity_update.cpp` writes `look.size` as a u16 at
`0x30` and then puts a four-character model name at `0x34` - the same field an
ordinary entity's name uses. LandSandBoat stores it as a `door_id` integer,
which is that name packed little-endian:

    812463711 = 0x306D365F = '_' '6' 'm' '0' = "_6m0"

and `_6m0` is exactly what the NPC's script is called in
`data/zones/windurst_waters/npcs.yaml`. Unpack the integer and you have the
model to draw.

## Finding the model

**Creatures - `1300 + modelid`.** One file holds the skeleton, the mesh and the
animation clips together, and the clips are named as a player's are: `idl0`,
`wlk0`, `run0`, `at00`, `btl0`, `ded0`. A rabbit's `wlk0` is its hop. Confirmed
by the skeletons' own names, not by arithmetic that happened to fit: model 269
is file 1569 with skeleton `usa ` (usagi, a rabbit); 340 is `shee`; 356 is
`kani`; 484 is `gob_`; 580 is `yagu`.

The 50000s hold creature **textures**, one file per skin, which is why they have
no skeletons and why recoloured monsters get files of their own.

**Equipment** ids arrive slot-tagged in the high nibble - `0x1000` head,
`0x2000` body, `0x3000` hands, `0x4000` legs, `0x5000` feet - and the model id
is the low twelve bits.

### Player characters: one block per race, 3176 apart

A playable race's files sit in a block that begins with its skeleton, and the
blocks are evenly spaced:

| race | base | race | base |
|---|---|---|---|
| Hume male | 7072 | Hume female | 10248 |
| Elvaan male | 13424 | Elvaan female | 16600 |
| Tarutaru male | 19776 | Tarutaru female | *see below* |
| Mithra | 23176 | Galka | 26352 |

Every step is **3176** files. Within a block, each slot has a window measured
from the base:

| slot | offset | ids |
|---|---|---|
| face and hair | 8 | 32 |
| headgear | 40 | 256 |
| body | 296 | 256 |
| hands | 552 | 256 |
| legs | 808 | 256 |
| feet | 1064 | 256 |

**The two Tarutaru share a block, except for their heads.** The step from
Tarutaru to Mithra is 3400, not 3176, and that extra 224 is what the female
gets: exactly 3176 past the Tarutaru base is a window of 32 face-sized files,
every one a different file from the male's. At 32 the sizes jump from forty
kilobytes to four hundred, so the window ends there rather than running on.

So a female Tarutaru is the male's skeleton, the male's gear windows, and her
own faces at offset 3176. Reading the sexes as sharing *everything* draws her
as a man, which is what happened.

The equipment windows are one flat list for both sexes. Their upper half looks
like a second body set - body 8 and body 136 are different files of different
sizes - and it is not: drawing from it puts the character in an ornate suit of
armour.

**Model 0 is nothing worn**, and the server sends it for an empty slot. It is
not the naked body: LandSandBoat gives a new character 8 in body, hands, legs
and feet, which is the starting clothing, and 0 in the head.

**Zones are a table, not a formula.** Model file = `100 + zone` holds for the
early zones and stops being true after them; files 525-528 would be "zones"
425-428, and two of those do not exist. Do not extend the arithmetic.

## A zone is more than one file

A city zone's own DAT is its shell. The insides of its buildings are separate
files, each a self-contained scene, and their placements are **already in zone
coordinates** - so they need loading, not aligning.

Those files say which zone they belong to. Every MZB chunk carries a
four-character tag: a zone's own reads `zone`, and its interiors share a family
tag `r_<n><city>`, numbered in zone order within that city.

| zone | | interiors | files | rooms matched |
|---|---|---|---|---|
| 230 | Southern San d'Oria | `r_1s` | 29 | 25 |
| 231 | Northern San d'Oria | `r_2s` | 13 | 13 |
| 232 | Port San d'Oria | `r_3s` | 5 | 5 |
| 233 | Chateau d'Oraguille | `r_4s` | 6 | 5 |
| 234 | Bastok Mines | `r_1b` | 14 | 13 |
| 235 | Bastok Markets | `r_2b` | 14 | 14 |
| 236 | Port Bastok | `r_3b` | 8 | 6 |
| 237 | Metalworks | `r_4b` | 1 | - |
| 238 | Windurst Waters | `r_1w` | 22 | 21 |
| 239 | Windurst Walls | `r_2w` | 7 | 6 |
| 240 | Port Windurst | `r_3w` | 7 | 5 |
| 241 | Windurst Woods | `r_4w` | 8 | 8 |
| 243 | Ru'Lude Gardens | `r_1j` | 10 | 10 |
| 244 | Upper Jeuno | `r_2j` | 10 | 10 |
| 245 | Lower Jeuno | `r_3j` | 14 | 14 |
| 246 | Port Jeuno | `r_4j` | 11 | 10 |
| 249 | Mhaura | `r_1m` | 5 | 5 |
| 250 | Kazham | `r_1k` | 7 | 7 |

Seventeen of these were found by matching, and they agree with the naming
convention exactly, which is what makes the convention trustworthy. Metalworks
is the one prediction: it is Bastok's fourth zone, `r_4b` exists and holds one
file, and no other family claims it - but the door test did not confirm it, so
treat it as unverified.

Still unassigned: `r_1t` (32 files, 21,530 placements - by far the largest),
`r_1a`, `r_2a`, `r_1n`, `r_1p`, `r_2g`, `r_3g`, `r_pa`.

### How the join is made

Matching on **position** does not work: every city is laid out around the origin
at a similar size, so any zone's bounding box contains every family's. Matching
on **textures** does not work either: an interior carries its own art and shares
almost none with the shell around it.

Doors do work. A building's interior is behind that building's door, the server
knows where every door in a zone stands, and an interior's placements are
already in zone coordinates - so a family belongs to the zone whose doors its
rooms are built around. Rank on matches multiplied by precision: on count alone
the largest family wins everywhere by coincidence, and on precision alone a
one-room family scores 100%.

`tools/subrooms.py --match <zone>` does this.

### What it is worth

Windurst Waters is `ROM/0/54` plus `ROM/2/18` through `ROM/2/38`: about 1,900
placements and 44,000 triangles against the 66,700 in the zone's own file. A
client that loads only the zone file draws roughly three-fifths of the world and
shows empty rooms with the furniture missing. Across all 26 families it is 252
files and 44,332 placements.

The interiors are named for what they are, in Japanese: `kazari` decoration,
`jutan` carpet, `tukue` desk, `tana` shelf, `hako` box, `tubo` pot, `isu` chair,
`hondana` bookshelf, `lanpu` lamp, `gaku` framed hanging, `kabe` wall, `yuka`
floor, `kaidan` stairs, `hasira` pillar, `mado` window, `kanban` sign.

**Do not use a hand-maintained list for this.** AltanaViewer's `zones.csv` is
what revealed that subrooms exist and is why any of this was found, but it names
ten files for Windurst Waters where the tag finds twenty-two - and one of the
twelve it misses is `ROM/2/34`, an ordinary shop interior with a bookshelf, a
framed hanging and a counter in it.

One caveat: `ROM/0/54` sits at the origin rather than out in the zone, so it is
in local coordinates like a Mog House, not zone coordinates like its siblings.
It is the one `r_1w` file the door test does not match.

## Chunk types

| type | contents |
|---|---|
| `0x05` | effect generators |
| `0x19` | small parameter blocks, 48-64 bytes - not identified |
| `0x1C` | MZB: placements, collision, lights, culling tree |
| `0x20` | texture |
| `0x21` | effect definitions, with embedded names (`light`, `bsmk`, `lf01`) |
| `0x29` | skeleton |
| `0x2B` | animation |
| `0x2E` | MMB: model |
| `0x2F` | skybox |
| `0x3D` | SeSep: sound |

Textures are **not** obfuscated; models and MZBs are, and need their key tables.
`0xA1` textures are block-compressed - `3TXD` is BC2 at 16 bytes a block, `1TXD`
is BC1 at 8 - and `0x81`/`0xB1` are paletted.

## Credits

**LandSandBoat**, for the look byte and for what the client really does with it,
and for being the reference for everything on the wire. Much of this page is
their knowledge written down, not ours.

**AltanaViewer**, for `List/Zones/zones.csv`, which named the first ten subrooms
and made the other twelve findable.

Thank you Teo!!!


## Two versions of one building: hidden-models.txt

Added 2026-09-03. Bastok Markets' placement table lists four `divmdl_00N`
models at one spot, -332.17 -12.01 -68.03, rotated 4.71 about y, byte for
byte alike apart from the name. `divmdl_000` and `_002` are the two halves
of a canvas tent (texture `s_ten`); `_001` and `_003` are the halves of the
stone auction house with roof and doors. Retail shows the building - the
auction houses were tents at launch and were rebuilt in a 2003 update - and
drawing all four at once gave stairs that shimmered from one angle and a pit
from another, with the character falling through the plaza beside them.

What tells the client which version to show is **not** in the placement
record (the four are identical, including the unidentified fields at 52 and
68), not in the MMB header, and not in the sub-map packets (0x00A's
`SubMapNumber` is the region within a zone; 0x010E/0x00EB/0x00F2 carry an
event's sub-map number). Until it is found, `renderer/assets/hidden-models.txt`
strikes named models per zone by hand, in the same `<zone dat>: words` shape
as `subrooms.txt`, and the loader reports how many placements it struck.
