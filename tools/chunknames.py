"""List the chunk ids in a DAT, or scan a range of file ids for a name prefix.

The index records chunk *types*; the four-character id is what actually tells
you what a file is for, so this fills that gap.
"""
import struct
import sys

from filetable import FileTable

INSTALL = "C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"


def chunks(data):
    off = 0
    while off + 16 <= len(data):
        packed = struct.unpack_from("<I", data, off + 4)[0]
        typ = packed & 0x7F
        length = ((packed >> 7) & 0x7FFFF) * 16
        if length < 16 or off + length > len(data):
            break
        yield typ, data[off:off + 4], data[off + 16:off + length]
        off += length


def ids(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    return [(t, i) for t, i, _ in chunks(data)]


def main():
    table = FileTable(INSTALL)
    if sys.argv[1].isdigit() and len(sys.argv) == 2:
        fid = int(sys.argv[1])
        for t, i in ids(table.path(fid)):
            print(f"  0x{t:02X}  {i!r}")
        return

    lo, hi = int(sys.argv[1]), int(sys.argv[2])
    want = sys.argv[3].encode() if len(sys.argv) > 3 else b""
    for fid in range(lo, hi):
        path = table.path(fid)
        if path is None:
            continue
        found = [(t, i) for t, i in ids(path) if i.startswith(want)]
        if found:
            names = " ".join(f"{i.decode('latin1')}:{t:02X}" for t, i in found[:6])
            print(f"{fid:6d}  {len(found):4d}  {names}")


if __name__ == "__main__":
    main()
