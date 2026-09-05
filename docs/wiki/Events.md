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
uint32   capacity - how many events
uint16   one unread word
         capacity - 1  where events 1..n begin, byte offsets into the code
         1             0xFFFF
         capacity      the ids the server names them by
         then the code
```

The index starts at **byte ten**, and that is the whole of why this looked
unsolved for so long. Read from byte twelve the runs come out a word short:
the terminator lands at `capacity - 2` in most blocks but not all, the offsets
are one fewer than they should be, and some entities appear to have events
with no offsets at all - which is exactly what was written here before.

Read from ten, in every block of every zone tried - 502 in Southern San
d'Oria, 308 in Northern San d'Oria, 173 in Valkurm Dunes, 76 in West Ronfaure -

- the terminator is at `capacity - 1`, without exception
- the offsets ascend, without exception
- every offset lands inside the code rather than past its end

**Event zero has no offset because it always begins at zero.** That is the
off-by-one: `capacity - 1` offsets for `capacity` events is not a shortage, it
is the first one being free.

### Which id goes with which event

In order: the *n*th id names the *n*th slot. The other reading - that the runs
are opposite ways round - was tried and is decisively worse. Ambrotien's five
events, the ones LandSandBoat says a server would call, come out at 657, 220,
210, 191 and 1 bytes in order, and at 4, 4, 4, 4 and 9 reversed.

The first slot is nearly always a single byte - 482 of 502 blocks in Southern
San d'Oria, 76 of 76 in West Ronfaure - and the last is never one. So an
entity's first event is usually a do-nothing default and the substance is
further in.

The retail files are the source of truth; LandSandBoat is how the reading was
*checked*, because it is the one place that says what number a server would
actually send:

| NPC | LSB says | code found |
|---|---|---|
| Glenne | 520, 513 | 154 and 49 bytes |
| Ailevia | 655, 615 | 31 and 82 bytes |
| Ambrotien | 2001, 2008, 2009, 2010, 2011 | 657, 220, 210, 191, 1 bytes |
| Coderiant | 583 | 1 byte |

Coderiant is the honest exception and is left in rather than explained away.
His named event holds a single byte, and the 413 bytes in his block sit under
event 19. Either his event really does nothing beyond the default - he is a
simple NPC - or something about him is still misread.

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
