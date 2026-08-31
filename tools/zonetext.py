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

        body = plain[offset + 6:offset + 2048]
        end = body.find(b"\0")
        if end >= 0:
            body = body[:end]
        lines.append(body.decode("ascii", "replace"))
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("zone", type=int)
    parser.add_argument("--install", default=INSTALL)
    parser.add_argument("--lines", type=int, default=10, help="how many dialogue lines to show")
    parser.add_argument("--event", type=int, help="dump one event script as hex")
    args = parser.parse_args()

    table = FileTable(args.install)

    scripts = events(table, args.zone)
    print("zone %d: %d event scripts" % (args.zone, len(scripts)))

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
