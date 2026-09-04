# Dialogue: what the people in a zone say

Every zone has one dialogue file holding its entire script - greetings, shop
patter, quest text, and the menus that go with them - as a numbered list.
Southern San d'Oria has 16,941 entries. An event names an entry by its number,
so **the numbering is the part that matters**.

    file id = 6420 + zone

## The format

A table of offsets, then the text. Both are obfuscated by XOR, neither is
compressed, and there is no key - one constant, the same for every zone.

| where | what |
|---|---|
| `0x00` | one dword, purpose unknown. Not the count. |
| `0x04` | the offset table: each dword `XOR 0x80808080` is an offset |
| `0x04 + firstOffset` | the text, each byte `XOR 0x80` |

**How many entries there are is never stated.** The table runs right up to the
text it points at, so the first offset is both where the text starts and how
long the table is: `count = firstOffset / 4`.

Offsets are relative to `0x04`, not to the start of the file, and an entry runs
until the next one begins.

### Inside an entry

The text is not plain: everything below `0x20` is a control code, and some of
them carry a parameter byte that is **not text**. Counted across Southern San
d'Oria's 16,941 entries:

| byte | count | meaning |
|---|---|---|
| `0x07` | 6206 | line break - or the next choice, once a menu is open |
| `0x0A` | 5186 | end of text, usually followed by the terminator |
| `0x0C` | 2486 | unknown, takes a parameter |
| `0x01` | 2914 | unknown, takes a parameter |
| `0x05` | 1967 | opens a substitution - `#`, `$` and friends follow |
| `0x04` | 1838 | opens a bracketed alternative, `[this/that]` |
| `0x0B` | 624 | everything after this is a menu |
| `0x1F` | 218 | **colour change**, next byte is which colour |
| `0x1E` | 121 | colour change, second form |
| `0x00`, `0x7F` | | the entry ends here |

`0x81` and `0x87` lead a two-byte character. Only `0x87 0xB2` and `0x87 0xB3`
matter in English - they are the quotes menu names are given in, "Map" and
"Markers" - and the rest render as nothing.

**The parameter bytes are the trap.** A colour code read as one byte leaves its
parameter behind as a letter, and since `0x1F 0x79` is a common pairing the
result is a stray `y` in front of coloured lines:

```
yYou will be able to use the Assist Channel until ...
```

which is not a typo in the game's text and not a yellow marker - it is `0x79`
being mistaken for a character. Both readers here had this bug.

### The menus are in the text

`0x0B` opens the list and each `0x07` after it starts the next choice, so
"Set this as current home point?" carries its own Yes and No:

```
Set this as current home point?<07><0B>Yes.<07>No.<7F>
```

That is the whole of an NPC menu - there is no separate format for one. The
bytes following `0x0B` are `Y`, `N`, `I`, `T` far more often than anything
else, which is what the first letters of Yes and No look like in a histogram.

So the menu choices are in the dialogue, not somewhere else - `0x0B` opens the
list and each `0x07` after it starts the next choice:

```
Set this as current home point?<07><0B>Yes.<07>No.<7F>
```

which is the question, then two options. That is the whole of an NPC menu.

## Reading it back

```
ffxi-dialogue <dialogue.DAT>            every entry, numbered
ffxi-dialogue <dialogue.DAT> 26         just entry 26
ffxi-dialogue <dialogue.DAT> --menus    only the entries offering a choice
```

```
[26] What will you do?
      0) Travel to another home point.
      1) Set this as your home point.
      2) Other settings.
      3) On second thought, never mind.
```

## What an event id is not

**An event id does not index this table**, and it is worth writing down how
that was settled so nobody spends the afternoon again.

LandSandBoat's scripts name the event each NPC starts, and its comments say
what the event should contain, which makes them checkable:

| NPC | starts event | the line is really entry | difference |
|---|---|---|---|
| Ailevia | 615 | 8168 | 7553 |
| Amaura | 645 | 7971 | 7326 |

Entry 8168 *is* "This is Southern San d'Oria. I know a thing or two about
these streets", exactly as the script's comment describes event 615 - so the
lines are all there and correctly decoded. But the two differences are not the
same number, so there is no offset and no arithmetic. Reading event 615 as
entry 615 gives "Trial: Use the prescribed weapon skill...", which belongs to
nothing in the zone.

Which entry an event shows is chosen by the zone's **event script**, and that
script has not been found. It is not in the zone model file, whose chunk types
are all accounted for (`0x20` textures, `0x2E` meshes, `0x36` zone lines,
`0x05` generators, `0x3D` sounds). The remaining candidates are the per-zone
blocks at `6120 + zone`, which has the same offset-table shape as this file,
and `6720 + zone`.

## What already works

Not every NPC needs a script. The server has two ways to make one talk:

- **TALKNUM** (`messageSpecial` in LandSandBoat) sends the line id outright,
  and the client looks it up here. This works today.
- **an event** (`startEvent`) sends an event id and expects the client to run
  the script. This does not.

So an NPC's silence is not one bug: it depends on which of the two the server
chose for them.
