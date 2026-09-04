# Events: where cutscenes live

The server never sends a cutscene. It sends **"play event 7 on entity
0x010E6003"** and expects the client to already have it — which is why an
unanswered event leaves a character standing invisible to everybody else, and
why this file has to be found before any cutscene can play.

## The file

**`5820 + zone`**, one per zone. See [File ids](File-Ids) for how an id becomes
a path; do not compute it.

```
uint32   how many blocks
uint32   the length of each, in order
         then the blocks, back to back
```

That is the whole container. No padding and nothing after the last block:
Southern San d'Oria is 4 + 502×4 + 911,840 = 913,852 bytes, which is exactly
the file size. If the lengths do not sum to the file, this is not the file you
think it is.

## A block belongs to an entity

Each block opens with the entity id it belongs to:

```
uint32   entity id, 0x1000000 | zone << 12 | index
...      the scripts
```

That is the same number the entity update carries, so a script is found from
what the server just said rather than by counting. Southern San d'Oria has 502
blocks: **501 distinct entity ids** in that zone's range, ascending, and one
more.

The odd one out is the first, filed under **`0x7FFFFFF0`**. That is the zone
itself — whatever belongs to the place rather than to anybody standing in it.
Every zone has exactly one and it is always block zero.

Sizes track how much an NPC has to say: 116 bytes for somebody with one line,
71KB for the busiest in the zone. Zone totals track the same thing — Southern
San d'Oria 913KB against Port San d'Oria's 321KB.

## What is inside one

Not decoded. A block opens with the id, a count, and a table of sixteen-bit
numbers, and past that it is bytecode — a machine that moves cameras, poses
actors, and calls up the dialogue in `6420 + zone`, which this project can
already read.

Getting a script out was the half that was missing. Running one is its own
problem, and the interesting question in it is how the bytecode names a line of
dialogue: if it uses the same ids the `0x036` TALKNUM packet does, then
everything needed to *show* a cutscene's words is already here, and only the
camera work is not.

Worth knowing before starting: three other files were ruled out on the way to
this one — the zone model, both dialogue tables, and the entity name table. The
scripts are in none of them.

## In this codebase

- `FfxiEventTable` — the container, and a block by entity id
- `MogHouse.Console events --zone <id> [--entity <id>] [--dump <file>]`
