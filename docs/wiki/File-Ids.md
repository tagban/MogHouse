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
| dialogue | `6420 + zone` | see [Dialogue](Dialogue) |
| building interiors | listed in [Object mapping](Object-Mapping) | a city is a shell plus separate files |

There are further per-zone blocks around `6120 + zone` and `6720 + zone` whose
purpose is not established. `6120 + zone` has the same offset-table shape as
the dialogue; `6720 + zone` is small and begins with the string `none`.

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
