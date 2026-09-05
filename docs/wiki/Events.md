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

## Which events an entity has, and where each begins

```
uint32   entity id
uint32   capacity
uint16   two unread words
uint16   where each event starts[capacity]   ends at 0xFFFF
uint16   the id the server names it by[capacity]
         then the bytecode
```

The two tables are parallel. Offsets are from the end of the index, which is
`0x0C + capacity * 4`.

**`0xFFFF` in the first table is a terminator, not a hole.** That is the whole
of why this took two attempts. Read as holes, a third of the blocks pointed
past their own end - entity `0x010E6004` offering `0x279` for a block 296 bytes
long. Read as a terminator, **all 1,626 blocks across seven zones resolve with
nothing left over**.

An entry whose *id* is `0xFFFF` has somewhere to start and no name to be called
by: reachable from inside a script rather than from the server.

Cross-checked against LandSandBoat, which is the only way to be sure a number
found in a file is a number a server would send:

- **Ambrotien** (`0x010E6062`): his script starts 2001, 2008, 2009, 2010 and
  2011, and his block holds all five - among thirty more the client knows and
  the server has not been taught yet.
- **Ailevia** (`0x010E607E`): her 655 and 615, starting at `0x69` and `0xBB`.

That the client knows more events than the server uses is worth expecting. The
DAT is retail's; a private server implements what it has got to.

## What is inside one

Not decoded. Past the index it is bytecode - a machine that moves cameras,
poses actors, and calls up the dialogue in `6420 + zone`, which this project
can already read.

The next question is how it names a line: if it uses the ids the `0x036`
TALKNUM packet already does, then everything needed to *show* a cutscene's
words is here and only the camera work is not.

## In this codebase

- `FfxiEventTable` — the container, and a block by entity id
- `MogHouse.Console events --zone <id> [--entity <id>] [--dump <file>]`
