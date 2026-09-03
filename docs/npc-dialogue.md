# Talking to NPCs

What happens when you click somebody, what works, and what stands between
here and a menu you can choose from.

Written 2026-09-03, against LandSandBoat's own packet structs and a live
server.

## The exchange

Four steps, three of which are ours.

| | |
|---|---|
| **C2S 0x01A** | `ACTION`, ActionID 0 = Talk. The UniqueNo and ActIndex are the **target's**, not ours - every other action this client sends happens to aim at the character themselves, so that distinction only became visible here. |
| **S2C 0x036 / 0x02A** | `TALKNUM` / `TALKNUMWORK`. A line id, not words. TALKNUMWORK adds four numbers and a 32-byte string the line's placeholders are filled from. |
| **S2C 0x032** | `EVENT`. Not a line but a whole cutscene, by id. Its field names lie: `EventNum` is the zone and `EventPara` is the event. |
| **C2S 0x05B** | `EVENTEND`. Until this arrives the character is `InEvent`, and a character in an event is not spawned for anybody else - present, addressable and invisible. |

An NPC volunteers nothing. The server runs its `onTrigger` only when asked,
which is why a client that never sends 0x01A can stand in front of a
shopkeeper indefinitely and hear nothing.

## What works

**Clicking somebody talks to them.** The renderer has always reported who was
clicked and the session has always had `TalkToAsync`, but until 2026-09-03
the only reader of the click was character select - in the world it was
collected and dropped. `WorldLoop` reads it now, resolves the ActIndex
through the tracker (a click comes back as a UniqueNo and the packet needs
both) and sends the talk.

**A line comes back as words.** `FfxiDialogueTable` reads the zone's dialogue
file - file id `6420 + zone`, XORed with 0x80 end to end - and indexes it by
the id the server sent.

**The top bit of the id is a flag.** From the server's own builder:

```cpp
packet.MesNum = (PEntity->objtype == TYPE_PC || !showName)
                ? (messageID + 0x8000) : messageID;
```

Set when a player is speaking or the line should appear with no name in front
of it. Taken as part of the id it lands 32,768 past the end of the zone's
table every time, which is what `(line 48555)` was: 48555 - 32768 is 15787,
an ordinary line in Southern San d'Oria. What made it findable rather than a
plausible missing id is that **no** zone table is large enough - the largest
in the game is 19,410 lines.

**A cutscene is ended rather than played.** `ClientFlow` answers 0x032 with
0x05B once per event and says so in the chat log. Without that the character
stays invisible to everyone else, which is what it used to do.

## What does not, and why

**Menus.** A conversation with choices is not a protocol feature. The server
sends an event id and nothing else; the dialogue, the choices and the
branching all live in an **event script in the client's own DAT files**,
which this project cannot read. The server only hears the answer, as
`EndPara` in the 0x05B.

So the missing piece is a script interpreter, not more packet work. Until
there is one, `EndPara` has nothing to put in it.

### Where the scripts are not

Ruled out on 2026-09-03, so nobody pays for it twice.

**Not in the zone's model DAT.** West Ronfaure's (`ROM/0/120.DAT`) holds
chunk types 0x00, 0x01, 0x05, 0x19, 0x1c, 0x20, 0x21, 0x25, 0x2e, 0x2f, 0x36
and 0x3d - terrain, models, textures and the effect generators, and nothing
that looks like bytecode.

**Not obviously in any neighbouring per-zone file.** A file id offset that
holds one file per zone shows up as a series; scanning 0 to 12000 for offsets
where more than 230 of zones 0..299 are present gives 1231, 3331, 3831, 4843,
5243 and 6985, alongside the known 100 (models) and 6420 (dialogue). Their
chunk histograms for zone 100 are in the session log; none was identified.
That scan is worth repeating with a wider zone range - it counts only zones
0..299, and a series that starts elsewhere would be missed.

### Also unhandled

**S2C 0x027 `TALKNUMWORK2`** is a third dialogue packet and nothing parses
it. Layout, from the server: UniqueNo, ActIndex, MesNum, Type (u16), Flags,
padding, Num1[4], String1[32], String2[16], Num2[8]. Its MesNum sits at the
same offset as TALKNUM's, so adding it to `FfxiNpcMessage` is small - it was
left out because nothing has been seen to send one yet, not because it is
hard.

**The placeholders.** TALKNUMWORK carries the numbers and the name a line's
`<number>` and `<item>` are filled from, and `FfxiNpcMessage` reads none of
them. A price or a count currently shows as the raw placeholder.

## Where to look next

1. **Find the event scripts.** Widen the per-zone file scan; check whether
   `EventPara` from a real 0x032 appears as an index in any of the candidate
   files. `!goto` an NPC with a known menu and watch which event fires.
2. **Read one script.** One shop or one guard is enough to learn the shape.
3. **Then the menu.** The renderer already draws forms with buttons and
   `ScreenHost.Ask` waits on one, so putting choices on screen is the part
   that is already built.

## Trying it

Stand next to somebody and click them. The log says who was asked:

```
talking to Ramaufont
```

and either their line appears in chat, or a note that a cutscene was skipped.
An NPC we have never had an entity update for cannot be addressed, and says
so - in practice that means out of range or already gone.
