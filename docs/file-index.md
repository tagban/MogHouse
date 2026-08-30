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

## File ids move between game versions

**This is the constraint that shapes everything else here.** The ids that
address content are not stable - they shift when Square Enix patches the game.
LandSandBoat carries ids for items and NPCs that are offsets into the DATs, and
those have to be revised each update for the same reason.

So no table mapping ids to content can ever be shipped with PortJeuno. Whatever
we know about a particular install is only true of that install, at that
version, until the next patch.

Three consequences worth stating plainly:

1. **The index is derived, never vendored.** `tools/buildindex.py` regenerates
   it from whatever is actually on disk, in about four minutes.
2. **A transcode cache must key on install state**, not on a version string, and
   rebuilding after a patch is structural rather than an optimisation. Ids
   moving underneath a stale cache would silently load the wrong asset, which is
   worse than failing.
3. **Names are the stable part, ids are not.** "West Ronfaure" does not change;
   which file id holds its geometry does. So the durable mapping is
   name -> content signature, with ids resolved fresh each time.

## Sources for names

Structure is derivable. Names are not - someone assigned them, and they have to
come from somewhere with clear terms.

| source | licence | what it gives |
| --- | --- | --- |
| [POLUtils](https://github.com/Windower/POLUtils) | **Apache 2.0** | container and encryption docs, text and data DATs, and MassExtractor |
| [LandSandBoat/UpdateExtractor](https://github.com/LandSandBoat/UpdateExtractor) | **MIT** | turns MassExtractor output into server data, and handles id shifts across updates |
| LandSandBoat | GPL-3.0 | zone ids and names, item and NPC ids |
| Altana Viewer | - | model tables, kept current with each game update |

The first two matter more than they look. **The whole existing pipeline for
re-deriving ids after a patch is permissively licensed**: POLUtils MassExtractor
does the extraction under Apache 2.0, and UpdateExtractor sanitises it under
MIT, explicitly handling the id shifts that come with each update.

What it does *not* cover is 3D. Its outputs are the client version string,
titles, status effects, zone text ids and item SQL - all server-side data.
Nothing permissive covers zone geometry, models or skeletons, which is why the
readers in `renderer/ffxi/` were written from the bytes.

So the division is: **the text and data side already has a permissive answer we
can use; the geometry side is ours to derive.**

LandSandBoat itself is GPL-3.0 and its ids are hardcoded and revised by hand each
update - `tools/client/animation_timing.py` carries base file ids commented as
coming from `FFXiMain.dll`'s data section. Which points at where the client's own
mapping actually lives, and at a more durable approach than hardcoding: read
those tables per install rather than pinning them per version.

LandSandBoat is **GPL-3.0**, so its files cannot be copied into PortJeuno. What
we can do - and what fits the pattern already used for the compression tables
and the MZB key table - is read the user's own LandSandBoat checkout when they
have one, rather than vendoring anything.

Its `sql/zone_settings.sql` holds 300 zones as `zoneid` and `name`, and is worth
reading for one detail alone:

```sql
INSERT INTO `zone_settings` VALUES (0,'127.0.0.1',54230,'unknown');
-- Demonstration Area from pre-release: Has no client side mesh, use wallhack to get around.
```

Zone 0 is the pre-release demonstration area, and "has no client side mesh" is
the same phenomenon as the ferry zones: nothing to collide with, so nothing to
stand on.
