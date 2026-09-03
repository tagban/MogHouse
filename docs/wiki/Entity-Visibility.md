# Entity visibility: status, flags and names

What in an entity update decides whether the thing is drawn and whether it is
named. Confirmed 2026-09-02 against LandSandBoat's own source and its zone
data, and against a retail client side by side in Southern San d'Oria.

Everything here is about the entity update packet, server to client id 0x00E
(NPCs and mobs) and 0x00D (players), one sub-packet per entity. Offsets are
from the start of the sub-packet.

## The four fields that matter

| Offset | Field | Set on | Meaning |
|---|---|---|---|
| 0x0A | send flags | every update | Which blocks of the packet are filled in. 0x02 is the "claim status" block, and only when it is set do the three rows below carry anything. 0x20 is a despawn. |
| 0x20 | status | claim-status block | The entity's spawn status, one byte. See below. |
| 0x21 | entity flags | claim-status block, NPCs and mobs | A 32-bit behaviour bitmask. See below. |
| 0x2B | name visibility | claim-status block | A byte about the name. See below. |

An update without the claim-status block is a position update and says
nothing about any of these. A client that reads them anyway reads zeros, and
a zero here means "normal" and "shown" - so a position-only update must never
be allowed to reveal something. MogHouse keeps the last value it was told.

## Status, at 0x20

From the server's generated `data/enums/status.h`:

| Value | Name | What the retail client does |
|---|---|---|
| 0 | Normal | drawn |
| 1 | Update | drawn |
| 2 | Disappear | **not drawn** |
| 3 | Invisible | **not drawn** |
| 4, 5, 7, 18 | unnamed | seen on nothing so far |
| 6 | CutsceneOnly | **not drawn** until an event shows it |
| 20 | Shutdown | despawning |

CutsceneOnly is the common one. In Southern San d'Oria's NPC data 366 of the
zone's NPCs are `status: cutscene_only` - the royal knights lined up on the
castle steps, the actors of every quest scene. The retail client draws none
of them; a client that ignores this byte draws a plaza full of people who are
not there.

MogHouse treats 2, 3 and 6 as hidden, and the hide-model flag below likewise.

## Entity flags, at 0x21

From `data/enums/entity_flags.h`:

| Bit | Name | Meaning |
|---|---|---|
| 0x0001 | InfoIcon | the entity carries an information icon |
| 0x0008 | HideName | no name over the head |
| 0x0020 | CallForHelp | a mob calling for help |
| 0x0080 | HideModel | **not drawn** |
| 0x0100 | HideHp | no HP bar for it |
| 0x0800 | Untargetable | cannot be clicked - warp triggers carry this |

Bits 0x0002, 0x0004 and 0x0010 are set on many NPCs (the zone's NPC data
defaults an NPC to 3, and 27 = 0x1B is the commonest value there) and have
no name in the server; whatever they mean, they do not hide anything.

HideName is honoured: the San d'Oria delivery girl Raminel carries 27, which
includes it, and has no name over her head in MogHouse. That is the server's
instruction rather than a fault.

## Name visibility, at 0x2B

From `data/enums/name_vis.h`:

| Bit | Name |
|---|---|
| 0x01 | Icon |
| 0x08 | HideName |
| 0x80 | GhostPhase |

Either HideName - here or in the entity flags - keeps the name off. Doors,
elevators and ships are never named regardless: their look block holds an id
or a name rather than an appearance, and the game shows their name in the
target box on a click, not floating over them.

## What has no look at all

An update whose look block is empty - no race, no model id - describes an
event trigger or a marker, not a body. MogHouse draws nothing for it. It
used to draw a pale stand-in body for anything it could not build a model
for, which put a row of blank figures on the castle steps: the cutscene
knights, whose status the client was not reading, and the markers, which had
never been meant to be seen.

## Races the client cannot yet build

Race is at look offset +3 (NPCs) and is 1 to 8 for the player races. The
zone data also uses 29, 30 and 31 - Mithra, Elvaan and Hume children. Raminel
is race 31, face 20, with every gear slot at model 20. MogHouse has no model
bases for those races and draws them as the grown race at 68% height until it
does; see the Windows handoff for where that stands.

## Where this lives in MogHouse

- `FfxiEntityUpdate.cs` reads the four fields and derives `IsHidden`,
  `HiddenByStatus`, `IsModelHidden`, `IsNameHidden`.
- `FfxiEntityTracker.cs` keeps them sticky across position-only updates and
  logs one line per entity whose race is above 8, with its status and flags.
- `LiveRadar.Publish` leaves hidden entities out entirely: no body, no name,
  no dot on the radar.
