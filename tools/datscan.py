"""Walk FFXI DAT containers and report their chunk structure.

Written to verify the container layout against real files rather than take it
on trust. Every offset here was checked against the retail DATs.

A DAT is a flat sequence of 16-byte-aligned chunks:

    char id[4]          four-character chunk name
    uint32 packed       type:7 | next:19 | shadow:1 | extracted:1 | ver:3 | virtual:1
    uint32 parent
    uint32 child

`next` is the chunk's total length in 16-byte units, header included. Type 0x00
closes the current directory, 0x01 opens one - so the file is a tree, flattened.
"""

import struct
import sys
from pathlib import Path

HEADER = 16

TYPE_NAMES = {0x00: "end", 0x01: "directory", 0x1C: "MZB", 0x29: "SK2", 0x2E: "MMB", 0x20: "DXT3?"}


def chunks(data):
    offset = 0
    while offset + HEADER <= len(data):
        name = data[offset:offset + 4]
        packed, parent, child = struct.unpack_from("<III", data, offset + 4)
        ctype = packed & 0x7F
        length = ((packed >> 7) & 0x7FFFF) * 16
        if length < HEADER:
            # A zero or undersized length would not advance - stop rather than spin.
            break
        yield offset, name, ctype, length, parent, child
        offset += length


def main(paths, want_type=None):
    for path in paths:
        data = path.read_bytes()
        found = []
        total = 0
        for offset, name, ctype, length, _parent, _child in chunks(data):
            total += 1
            if want_type is None or ctype == want_type:
                found.append((offset, name, ctype, length))
        if found and want_type is not None:
            print(f"{path}  ({total} chunks)")
            for offset, name, ctype, length in found:
                label = TYPE_NAMES.get(ctype, f"0x{ctype:02X}")
                print(f"    +{offset:#08x}  {name!r:12} {label:10} {length} bytes")
        elif want_type is None:
            print(f"{path}: {total} chunks, {len(data)} bytes")


def collect(args, limit):
    """Accept files or directories. Paths with spaces are the norm here (the
    retail install lives under Program Files), so globbing is done in python
    rather than left to the shell."""
    files = []
    for arg in args:
        path = Path(arg)
        if path.is_dir():
            files.extend(sorted(path.rglob("*.DAT")))
        else:
            files.append(path)
        if limit and len(files) >= limit:
            break
    return files[:limit] if limit else files


if __name__ == "__main__":
    target = int(sys.argv[1], 0) if len(sys.argv) > 1 and sys.argv[1] != "-" else None
    limit = int(sys.argv[2])
    main(collect(sys.argv[3:], limit), target)
