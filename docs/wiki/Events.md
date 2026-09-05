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

## Which events an entity has

```
uint32   entity id
uint32   capacity
uint16   two unread words
         capacity * 2 words of index:
           where each event begins
           0xFFFF
           the ids the server names them by
         then the bytecode
```

**The terminator separates the two runs, not their lengths.** That is the part
that took three attempts. A block can have no offsets at all and still have
events - Coderiant has one event and no offsets whatever - so reading the ids
at a fixed distance from the start works for some people and not others.

The retail files are the source of truth. LandSandBoat is how the reading was
*checked*, because it is the one place that says what number a server would
actually send:

| NPC | LSB says | in the block |
|---|---|---|
| Coderiant | 583 | yes |
| Glenne | 520, 513 | yes |
| Ailevia | 655, 615 | yes |
| Ambrotien | 2001, 2008, 2009, 2010, 2011 | all five |

Across a whole zone, `DefaultActions.lua` names an event for 38 of Southern San
d'Oria's people and **32 are exactly where this puts them**. Three are not
found and three could not be matched to a block by name. Where the two
disagree it is not settled that the file is the one in the wrong: the DAT is
retail's own and a private server implements what it has got to.

**Where each event begins is not solved.** The offsets run before the
terminator and there are fewer of them than there are events, so they are not
one per event, and guessing at the pairing would be worse than admitting it.

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
