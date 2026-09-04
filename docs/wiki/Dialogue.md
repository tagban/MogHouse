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

Three bytes matter, after the `XOR 0x80`:

| byte | meaning |
|---|---|
| `0x07` | line break |
| `0x0B` | everything after this is a menu |
| `0x7F` | the entry ends here, whatever follows |

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

## What is still missing

The text is readable; **which entry to show is not.** That is decided by the
zone's event script, which has not been found yet - it is not in the zone model
file, whose chunks are all accounted for (`0x20` textures, `0x2E` meshes, `0x36`
zone lines, `0x05` generators, `0x3D` sounds).

Until then the client can read every line an NPC might say and cannot tell
which one they would say. Packet `0x032` carries an event id, and whether that
id indexes this table directly is untested.
