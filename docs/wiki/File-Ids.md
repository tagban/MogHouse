# File ids: what is per-zone and what is shared

FFXI addresses its data by file id, resolved through the client's own file
table (`VTABLE.DAT` and `FTABLE.DAT`) to a `ROM*/x/y.DAT` path. **Do not
compute the path** - the mapping is not arithmetic and the "ROM number times
something" shortcuts stop working past the early zones.

The ids themselves, though, are often arithmetic.

## Per-zone

| what | id | notes |
|---|---|---|
| zone model | `100 + zone` | geometry, textures, generators, sounds, zone lines |
| dialogue, English | `6420 + zone` | see [Dialogue](Dialogue) |
| dialogue, Japanese | `6120 + zone` | the same format and the same entry count |
| entity names | `6720 + zone` | who is in the zone, and their ids |
| building interiors | listed in [Object mapping](Object-Mapping) | a city is a shell plus separate files |

`6120` and `6420` are the same table twice, once per language - Southern San
d'Oria has 16,940 entries in both, the first in Shift-JIS and the second in
English. So a zone's script is translated by swapping which file is read,
which is why an NPC speaks the language the client was installed in rather
than the one the server was built in.

## The entity name table

`6720 + zone` is a flat array of 32-byte records and nothing else - no header,
no offsets:

| bytes | what |
|---|---|
| 0-27 | the name, null padded |
| 28-31 | the entity's id |

The id is `0x1000000 + (zone << 12) + index`, which is exactly the scheme the
server numbers its own entities with: Southern San d'Oria's first NPC is
`0x010E6001`, and `(0x010E6001 - 0x1000000) >> 12` is 230.

That means **the client already knows who is in a zone before the server says
anything.** Southern San d'Oria lists 451 named entities - Ceraule, Aubejart,
Rolandienne, Ailevia - and the server need only send an id for one of them to
be named correctly.

The first record is always `none` with id 0, which is what an index of zero
resolves to.

**`100 + zone` is only true for the early zones.** It holds long enough to look
like a rule and then quietly stops - use the file table.

## Per-race

| what | id |
|---|---|
| skeleton and animations | the race base - 7072 hume male, 26352 galka |
| equipment models | race base + slot offset |

The full table is in [characters.md](../characters.md). Bones inside those
skeletons are found by shape, not index - see [Skeletons](Skeletons).

## Per-model

| what | id |
|---|---|
| a creature or NPC model | `model + 1300` |

## Items, and the languages they come in

Item data is not per-zone and not per-id-formula: it is a couple of dozen large
tables, each holding a run of items with **the icon and the text together**.
Names are obfuscated by rotating every byte right five bits - not XOR, unlike
the dialogue - and every table contains the marker `icon`, which is how they
are found at all.

There are 23 of them, 219 MB, and they come one set per language:

| where | size | language |
|---|---|---|
| `ROM/0/4-6`, `ROM/118/106-108` | 12-19.5 MB each | English |
| `ROM/176/101-102` | 12 MB each | German |
| `ROM/178/41`, `ROM/178/43` | 12-18 MB | French |
| `ROM/301/114-117` | 4.5 MB each | one per language |

So an item's name, its description and its picture are all in the same record,
and translating the game means shipping the pictures again with it.

## Shared by everything

These are not addressed by file id at all. They sit in the install as ordinary
files, one copy, used by every zone:

| what | where |
|---|---|
| sound effects | `sound/win/se/se<nnn>/se<nnnnnn>.spw` |
| music | `sound/win/music/data/*.bgw` |

A zone does not carry its own sounds; it *declares* which of the shared ones it
uses, as `0x3D` chunks. That is also how a creature says what noises it makes -
see [Audio formats](Audio-Formats). So the same worm sound, 17024, is one file
played by every zone holding a worm, and the effect library a zone appears to
carry is a reference rather than a copy.

## Reading it back

`tools/filetable.py` resolves an id to a path, and every CLI here takes the
resolved path:

```python
from filetable import FileTable
FileTable(install).path(6420 + 230)   # Southern San d'Oria's dialogue
```
