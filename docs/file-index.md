# Finding things in the retail install

The install has no manifest. Content is addressed by a flat **file id**, and
what a file actually contains is only discoverable by opening it. Two things are
needed to work with it: resolving an id to a path, and knowing what is inside.

## Resolving a file id

Two tables at the install root, one entry each per id:

| file | entry | meaning |
| --- | --- | --- |
| `VTABLE.DAT` | `u8` | ROM number holding it, `0` if not installed |
| `FTABLE.DAT` | `u16` | `(directory << 7) \| file` |

giving `ROM{rom}/{directory}/{file}.DAT`, where ROM 1 is the folder called plain
`ROM` rather than `ROM1`.

The bit split was derived rather than assumed - candidate shifts were tried
against the install and scored on how many ids landed on files that exist:

| shift | resolved |
| --- | --- |
| **7** | **82,912 / 82,912 = 100.0%** |
| 8 | 50.3% |
| 9 | 27.6% |

100% is what makes it certain. `tools/filetable.py` implements it.

This install has **109,701 file ids, of which 82,912 are present.** The other
26,789 are content for expansions or regions not installed, and `VTABLE` says so
with a zero.

## Knowing what is in them

`tools/buildindex.py` walks every installed id, records the chunk types it
holds, and notes every MZB's zone id. The result is written as JSON so it can be
diffed between game versions - which is also how a transcode cache would decide
what to rebuild after PlayOnline patches.

The index is **not committed**. It is derived from a particular install, it is
large, and it is regenerable in about four minutes.

A note on scale, from the first 3,000 files alone: 24,313 models, 959 skeletons,
350 zone chunks. A single DAT mixes content types freely - "a zone file" is not
a thing, only "the chunks of the type I want, wherever they happen to live".

## Why build our own

The client resolves zone ids through its own tables, which we do not have. What
we can do is index by what is actually in the files, then attach names to those
ids from public research. Zone chunk ids are four characters - `r_3b`, `f_sa`,
`ship` - and the mapping from those to zone names is exactly the kind of thing
other projects have already worked out and published.

That split matters: **the structure comes from the data, the names come from
research.** Anything derived from the install is a fact about bytes and can be
regenerated; anything that is a name someone assigned needs a source.
