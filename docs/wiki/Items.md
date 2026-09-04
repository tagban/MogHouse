# Items: the four DATs, and what the server says about them

Everything the client knows about an item without asking the server lives in
four files. Everything about *which* items you have comes from the server, and
carries nothing but ids.

## The four files

They are the same four files twice - once in English, once in Japanese - and
they carve the id space into fixed ranges with a gap in the middle:

| contents | English | Japanese | records | first id | header |
|---|---|---|---|---|---|
| general | 73 | 4 | 4096 | `0x0000` | `0x18` |
| usable | 74 | 5 | 4096 | `0x1000` | `0x1C` |
| weapon | 75 | 6 | 6656 | `0x4000` | `0x38` |
| armour | 76 | 7 | 6144 | `0x2800` | `0x2C` |

Ids resolve to paths through the client's own file table - see
[File ids](File-Ids). Do not compute them.

`0x2000` to `0x27FF` is not in any of them.

A record is a **fixed `0xC00` bytes** and its position is the item id minus
that file's first id. There is no index: id 12579 is record 2339 of file 76,
and that is the whole lookup.

## The encryption

Every byte of a record is stored **rotated right by five bits**. That is the
entire scheme - no key, no per-file variation, nothing derived from the id:

```
plain = (stored >> 5) | (stored << 3)
```

Record 0 of each file is a blank placeholder, so read record 1 to learn the
file's first id. It will be one more than it.

## Inside a record

```
0x00  uint32  item id            same as (position + first id)
0x04  uint16  flags
0x06  uint16  stack size
0x08  uint16  type               1 general, 4 weapon, 5 armour, 7 usable, 8 crystal, 10 furnishing
0x0A  uint16  valid targets
0x0E  uint16  level              equipment only
0x10  uint16  slots              equipment only, bitmask; bit 0 main hand, bit 15 back
0x12  uint16  races              equipment only, bitmask
0x14  uint32  jobs               equipment only, bitmask; bit 1 is warrior
0x1C  uint16  damage             weapons only
0x1E  uint16  delay              weapons only
0x22  uint16  skill              weapons only; 3 sword, 11 club, 12 staff
```

The header ends where the string table starts, which is the "header" column
above and varies by file: a crystal has less to say than a sword. It does not
vary within a file.

**The equipment model id is not in here.** The server sends it in the entity's
`look_t`, so the client never needs an item-to-model mapping to draw someone
wearing something.

### The string table

```
<uint32 count>  then count x { uint32 offset; uint32 flags }
```

Offsets are relative to the **start of the table**, and point at a block:

```
<uint32 attributeCount>  then attributeCount x 24 bytes  then the text, NUL terminated
```

The attribute count has only ever been seen as 1, and its 24 bytes as zero.

The count is always 5, and always in this order:

| # | what | example |
|---|---|---|
| 0 | display name | `Simple Bed` |
| 1 | blank | |
| 2 | log name, singular | `simple bed` |
| 3 | log name, plural | `simple beds` |
| 4 | description | `Furnishing:\nA crude bed of simple construction.` |

`0x0A` is a newline. The English files are otherwise ASCII apart from the
symbols below.

### Resistances are symbols, not words

A description writes an elemental resistance as `0xEF` followed by one byte,
`0x1F` to `0x26`, which is **Fire, Ice, Wind, Earth, Lightning, Water, Light,
Dark** in the game's usual order. The retail client draws those as the small
coloured orbs; there is no text in the file to fall back on.

Confirmed against the server rather than guessed. Scorpion Harness reads

```
DEF:40 HP+15 [EF20]-20 [EF24]+15 [EF26]+15 Accuracy+10 Evasion+10
```

and LandSandBoat gives that item ice -20, water +15, dark +15.

A few other high bytes lead a two-byte pair borrowed from Japanese
punctuation - `0x81 0x60` a wave dash, `0x81 0x45` a bullet, `0x81 0xCB` and
`0x81 0xCC` the arrows an enchantment uses for "here to there".

### The icon

At `0x280`, and an ordinary Windows DIB with a label stuck on the front:

```
0x280  uint32  size, 2105 for a 32x32
0x284  uint8   flag
0x285  char[16] a label like "armor   12568   "
0x295  BITMAPINFOHEADER, 40 bytes: 32 x 32, 8 bits, no compression
0x2BD  256 palette entries, BGRA
0x6BD  1024 pixels, one byte each
```

Rows run **bottom-up**, as DIB rows do. Alpha is the usual FFXI half scale
where `0x80` is opaque, so double it.

`2105 = 1 + 16 + 40 + 1024 + 1024` exactly, which is a good check that the
record was decoded rather than merely read.

## What the server sends

The server never sends an inventory. It sends a stream of one-slot updates
with no count in front of it, and the same stream again one packet at a time
whenever anything changes.

| packet | | what |
|---|---|---|
| `0x01C` | S2C | how many slots each of the 18 containers has |
| `0x01D` | S2C | a container finished, or all of them did |
| `0x01E` | S2C | a slot's quantity changed - **no item id in it** |
| `0x01F` | S2C | an item in a slot: id, count, container, slot |
| `0x020` | S2C | the rest: price, and 24 bytes of augments/signature/charges |
| `0x050` | S2C | what is worn in one equipment slot |
| `0x050` | C2S | wear this |

`0x01D` arrives twice over: once per container as that container finishes,
with state 0 and the container's own id in the byte the struct calls padding;
then once at the end with state 1 and 18 - `MAX_CONTAINER_ID` - in the same
byte. **State 0 is not an error**, it is a bag reporting in.

A quantity of zero in `0x01F` is how the server empties a slot. There is no
separate removal packet.

### Equipment is a pointer, not a thing

`0x050` names a **place**: container 0, slot 12. To know what is worn, look
that up in the bags - which means the bags have to arrive before what you are
wearing means anything. Slot 255 (`ERROR_SLOTID`) means nothing is worn.

Changing gear is three bytes and **no item id**:

```
uint8 itemSlot     where it is now, or 255 to take something off
uint8 equipSlot    SLOTTYPE: 0 main ... 15 back
uint8 container    CONTAINER_ID
```

So the server can only equip what it agrees is in that slot. A client with a
stale inventory will equip whatever has since taken the place.

The server refuses the packet while the character is in an event or under a
status that blocks it, and refuses any container but the inventory and the
wardrobes unless it has been configured otherwise.

## In this codebase

- `FfxiItemTable` - the four DATs: names, descriptions, stats, icons
- `FfxiInventory` - the packets above
- `FfxiInventoryTracker` - the running picture the packets build up
- `MogHouse.Console items --find | --id | --export`
