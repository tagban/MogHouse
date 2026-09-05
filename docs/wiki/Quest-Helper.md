# Quest helper: what the files can tell us, and what they cannot

A modern MMO tells you where to go next. FFXI never did - it tells you what
somebody said, once, and expects you to remember it. This is a scope for
building the missing half, and the first thing it has to settle is where the
information would come from, because the honest answer is "not mostly from the
DATs".

## The short answer

**A quest's steps are not in the client's files.** The DATs hold the words and
the cutscenes; the *order* of things, and which NPC advances which stage, is
server logic. A helper therefore needs a repository of its own, and most of
this document is about how to build one without inventing it and without
copying it.

What the client already holds, verified:

| what | where | good for |
|---|---|---|
| every named entity in a zone, and its id | `6720 + zone` | naming a target before the server mentions it |
| every line the zone can say | `6420 + zone` | matching text to a step |
| which entities own a cutscene, and its ids | `5820 + zone` | narrowing "who is involved" |
| where an entity stands | the server, on spawn | putting a marker on the radar |

What the client does not hold:

- which quest a line or event belongs to
- the order of a quest's stages
- what a stage wants you to do

## Why the DATs stop short

The dialogue table is a flat array of strings by id. "Rosel the Armorer" and
"The Rescue" are both in Southern San d'Oria's, but they are in it the way any
other sentence is - as something an NPC says. Nothing marks them as quest
names, and nothing links them to a stage. The event table is keyed by entity
and lists event ids, so it says *that* Rosel has cutscenes, never that one of
them starts a quest.

So the DATs answer "who is here, what can they say, do they have a scene" and
stop.

## What the server says

Packet `0x056` carries the quest and mission log - which are open, which are
complete. That is the state, and it is enough to know *which* quest a player
is on and at what stage. It is not enough to know where to go, because the
server does not send that either; its own scripts hold the coordinates and it
never tells the client.

## The shape of a leg

From a server's own quest logic, a step reduces to very little:

    quest, stage  ->  zone, NPC, position, and a sentence saying why

That is the record the repository needs. Everything else - the NPC's name, its
model, the zone's name, the walking route - the client can already work out
from an entity id, because `6720 + zone` names it and the id encodes the zone.

**So a repository entry can be as small as `(quest, stage) -> entity id`.** The
name comes from the DAT and the position from the zone. That matters: it keeps
the repository small, it keeps it free of anybody else's data, and it stays
correct if a server moves an NPC.

## Where the entries come from

Three sources, in increasing order of how much they cost and decreasing order
of how much they are worth.

**1. Watch the player.** The client sees the quest log change and it sees who
was being talked to when it changed. That pair is exactly a leg, observed
rather than authored, and it needs no outside data at all. A player finishing a
quest writes the route for the next player. This is the one to build first: it
is small, it is ours, and it improves on its own.

**2. Read the events.** A quest stage is usually an event on an entity. The
event table already gives entity to event ids; when the event bytecode is
understood (see [Events](Events), where it currently stops) it should be
possible to say which events set which flags, and a flag is what a quest stage
is. This turns observation into derivation and would fill in quests nobody has
played yet.

**3. Author the rest by hand,** for the quests neither of the above reaches.

A server's scripts can be read to *check* an entry - to ask "is this the right
NPC" - and that is all they should be used for. LandSandBoat is GPL-3.0 and
MogHouse is MIT; the facts are facts, but their data files are theirs, so
entries get derived or observed here and validated against theirs, never
copied from them.

## What it would look like

- a **journal panel**, listing open quests from `0x056` with their names
- per quest, the legs known for the stage the player is on
- the current leg's target as a **radar marker** and a distance, since the
  radar already draws entities and already knows where they are
- when the target is in another zone, the zone line to take - the client
  already reads zone lines, so "go to Valkurm Dunes" can become "leave by the
  east gate"

## What to build first

The observation recorder, on its own, headless. No panel, no marker: just
watch `0x056` against who was last spoken to, and write entries to a file. It
is a few days of work, it cannot make anything worse, and after it exists the
question "do we have enough to draw a helper" answers itself with data instead
of an argument.

The two open dependencies, both already on the list for other reasons:

- **the event bytecode**, which is what would make source 2 possible
- **quest names**, which are not yet located in a DAT - they may be in a table
  this project has not found, or the log may be assembled from strings the
  server sends
