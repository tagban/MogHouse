"""Reads a zone's dialogue and event scripts out of the client's own files.

Every zone has four tables, and the file id of each is the zone number plus a
base:

    model   100 + zone     the geometry, which the renderer already reads
    events  5820 + zone     one script per event
    dialog  6420 + zone     the lines those scripts say
    npcs    6720 + zone     entity names, which the renderer already reads

The bases were derived by resolving three known files for zone 235 and
subtracting, then checked against other zones.

DIALOGUE is obfuscated with a flat xor 0x80 - nothing more - and reads:

    u32          unknown header
    u32[]        offset per line, starting at 4; the first one ends the table
    ...          each line: six bytes of header, then NUL-terminated text

EVENTS are not obfuscated at all:

    u32          how many scripts
    u32[count]   the size of each
    ...          the scripts, back to back

The size table accounts for the file exactly - count, sizes and blocks sum to
the byte, which is what says this layout is right rather than merely plausible.
"""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from filetable import FileTable

INSTALL = "C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"

EVENT_BASE = 5820
DIALOG_BASE = 6420
NPC_BASE = 6720


def decode(raw):
    """One line of DAT text as something printable.

    0x07 is a line break within one entry, not a separator between entries.
    0x81 leads a two byte sequence the retail client renders as nothing in
    English text, so it is dropped rather than shown as two replacement
    characters in the middle of a word.
    """
    out = bytearray()
    i = 0
    while i < len(raw):
        byte = raw[i]
        if byte == 0x07:
            out.append(0x0A)
        elif byte in (0x81, 0x87) and i + 1 < len(raw):
            # Two byte sequences. 0x87 0xB2 and 0x87 0xB3 are the quotes
            # the menus are named in - "Map", "Markers" - and are worth
            # keeping; the rest render as nothing in English text.
            pair = raw[i + 1]
            if byte == 0x87 and pair in (0xB2, 0xB3):
                out.append(0x22)
            i += 1
        elif byte >= 0x20:
            out.append(byte)
        i += 1
    return out.decode("ascii", "replace")


def dialogue(table, zone):
    """Every line of text a zone can say, in order."""
    path = table.path(DIALOG_BASE + zone)
    if not path or not Path(path).exists():
        return []

    plain = bytes(b ^ 0x80 for b in Path(path).read_bytes())
    first = struct.unpack_from("<I", plain, 4)[0]
    count = (first - 4) // 4

    lines = []
    for i in range(count):
        offset = struct.unpack_from("<I", plain, 4 + i * 4)[0]
        if not 0 < offset < len(plain):
            lines.append("")
            continue

        # Four bytes of header, then the text, then 0xFF - which the
        # file-wide XOR turns into 0x7F. Checked against LandSandBoat's own
        # IDs.lua for this zone, which quotes by id the strings it uses:
        # 14, 32, 4140 and 4702 all come back character for character. A
        # six byte header ate the first two letters of every line, and a
        # NUL terminator ran past the end of the text into what follows.
        body = plain[offset + 4:offset + 4096]
        stops = [at for at in (body.find(bytes([0])), body.find(bytes([0x7F]))) if at >= 0]
        lines.append(decode(body[:min(stops, default=len(body))]))
    return lines


def events(table, zone):
    """Each event script as its own block of bytes."""
    path = table.path(EVENT_BASE + zone)
    if not path or not Path(path).exists():
        return []

    data = Path(path).read_bytes()
    count = struct.unpack_from("<I", data, 0)[0]
    sizes = struct.unpack_from("<%dI" % count, data, 4)

    blocks = []
    offset = 4 + count * 4
    for size in sizes:
        blocks.append(data[offset:offset + size])
        offset += size
    return blocks


def event_entities(blocks):
    """Which events each entity in the zone has.

    A block past the first is one entity's events:

        u32        the entity id, 0x1000000 | zone << 12 | targid
        u32        how many events it has
        u16[count] their ids
        u16        0xFFFF, ending the list
        ...        the scripts themselves, still undecoded

    Checked against every block of Bastok Markets: all 210 have 0xFFFF exactly
    where the count says it should be, which is what makes this a layout rather
    than a guess that fitted one block.

    Block 0 is not an entity. It opens 0x7FFFFFF0 and is far larger than any
    other - the zone's own script rather than anybody's.
    """
    found = {}
    for block in blocks[1:]:
        if len(block) < 12:
            continue

        entity, count = struct.unpack_from("<II", block, 0)
        if count == 0 or 8 + count * 2 + 2 > len(block):
            continue
        if struct.unpack_from("<H", block, 8 + count * 2)[0] != 0xFFFF:
            continue

        found[entity] = list(struct.unpack_from("<%dH" % count, block, 8))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("zone", type=int)
    parser.add_argument("--install", default=INSTALL)
    parser.add_argument("--lines", type=int, default=10, help="how many dialogue lines to show")
    parser.add_argument("--event", type=int, help="dump one event script as hex")
    parser.add_argument("--entities", action="store_true", help="which events each entity has")
    args = parser.parse_args()

    table = FileTable(args.install)

    scripts = events(table, args.zone)
    print("zone %d: %d event scripts" % (args.zone, len(scripts)))

    if args.entities:
        names = {}
        npc = table.path(NPC_BASE + args.zone)
        if npc and Path(npc).exists():
            raw = Path(npc).read_bytes()
            for off in range(0, len(raw) - 31, 32):
                ident = struct.unpack_from("<I", raw, off + 28)[0]
                name = raw[off:off + 28].split(b"\0")[0].decode("ascii", "replace")
                if ident:
                    names[ident] = name

        for entity, ids in sorted(event_entities(scripts).items()):
            print("  0x%03X %-22s %s" % (entity & 0xFFF, names.get(entity, "")[:22], ids))
        return

    if args.event is not None:
        if not 0 <= args.event < len(scripts):
            raise SystemExit("no event %d in this zone" % args.event)
        block = scripts[args.event]
        print("event %d, %d bytes:" % (args.event, len(block)))
        for row in range(0, min(len(block), 256), 16):
            chunk = block[row:row + 16]
            print("  +%04X  %-47s  %s" % (row, " ".join("%02x" % b for b in chunk),
                                          "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
        return

    text = dialogue(table, args.zone)
    print("zone %d: %d dialogue lines" % (args.zone, len(text)))
    shown = 0
    for i, line in enumerate(text):
        if line.strip():
            # Windows consoles are not UTF-8, and one stray accent should not
            # end a dump that is otherwise fine.
            safe = line[:110].replace(chr(10), " / ").encode("ascii", "replace").decode("ascii")
            print("  [%5d] %s" % (i, safe))
            shown += 1
            if shown >= args.lines:
                break


if __name__ == "__main__":
    main()
