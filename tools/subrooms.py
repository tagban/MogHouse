#!/usr/bin/env python3
"""Work out which DAT files hold each zone's building interiors.

A city zone's own DAT is only its shell. The insides of its buildings live in
separate files, each a self-contained scene whose placements are already in
zone coordinates - so they need no alignment, only loading.

Those files identify themselves. Every MZB chunk carries a four-character tag:
a zone's own reads `zone`, while its interiors all share a family tag like
`r_1w` for Windurst Waters, `r_1b` for Bastok, `r_1s` for San d'Oria. Grouping
by that tag recovers the whole set, and matching each family's bounding box
against a zone's recovers which zone it belongs to.

    python tools/subrooms.py --scan            # find every r_* file (slow, once)
    python tools/subrooms.py --match 238       # which files belong to a zone

Do not use a hand-maintained list for this. AltanaViewer's zones.csv names ten
files for Windurst Waters; the tag finds twenty-two, and the twelve it misses
include ROM/2/34, which is the room a player standing at (-57, 93) is inside.
"""
import argparse
import collections
import json
import os
import struct
import subprocess
import sys

INSTALL = r"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATDUMP = os.path.join(ROOT, "build-renderer", "ffxi-datdump.exe")
KEYTABLE = os.path.join(ROOT, "keys", "mzb_key_table.bin")
MANIFEST = os.path.join(HERE, "subrooms.json")
HEADER = 16


def mzb_tags(path):
    """Every MZB tag in one DAT. Tags are plaintext; the body is not."""
    try:
        if not (5_000 <= os.path.getsize(path) <= 40_000_000):
            return []
        raw = open(path, "rb").read()
    except OSError:
        return []
    tags, off = [], 0
    while off + HEADER <= len(raw):
        packed = struct.unpack_from("<I", raw, off + 4)[0]
        kind, length = packed & 0x7F, ((packed >> 7) & 0x7FFFF) * 16
        if length < HEADER or off + length > len(raw):
            break
        if kind == 0x1C:
            tags.append(raw[off:off + 4].decode("ascii", "replace"))
        off += length
    return tags


def scan():
    found = collections.defaultdict(list)
    for dirpath, _, files in os.walk(INSTALL):
        if "ROM" not in dirpath:
            continue
        for name in files:
            if not name.upper().endswith(".DAT"):
                continue
            path = os.path.join(dirpath, name)
            for tag in mzb_tags(path):
                found[tag].append(path)
    return {tag: sorted(set(paths)) for tag, paths in found.items()}


def bounds(path, scratch):
    """A file's placement bounding box, via the renderer's own decryption."""
    env = dict(os.environ, MOGHOUSE_MZB_DUMP=scratch, MOGHOUSE_FFXI_KEYTABLE=KEYTABLE)
    subprocess.run([DATDUMP, path], env=env, capture_output=True)
    try:
        buf = open(scratch, "rb").read()
    except OSError:
        return None
    count = struct.unpack_from("<I", buf, 4)[0] & 0x00FFFFFF
    ends = struct.unpack_from("<I", buf, 20)[0]
    if not count:
        return None
    stride = 0x64 if 32 + count * 0x64 == ends else (ends - 32) // count
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for i in range(count):
        point = struct.unpack_from("<3f", buf, 32 + i * stride + 16)
        for axis in range(3):
            lo[axis] = min(lo[axis], point[axis])
            hi[axis] = max(hi[axis], point[axis])
    return {"count": count, "lo": lo, "hi": hi}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scan", action="store_true", help="rebuild the manifest (walks the whole install)")
    ap.add_argument("--match", type=int, metavar="ZONE", help="list the interior files for a zone")
    ap.add_argument("--scratch", default=os.path.join(HERE, "_mzb.tmp"))
    args = ap.parse_args()

    if args.scan:
        tags = scan()
        interiors = {t: p for t, p in tags.items() if t.startswith("r_")}
        out = {}
        for tag, paths in sorted(interiors.items()):
            entries = []
            for path in paths:
                box = bounds(path, args.scratch)
                if box:
                    entries.append({"path": path[len(INSTALL) + 1:], **box})
            out[tag] = entries
            print("  %-6s %2d files, %d placements" % (tag, len(entries), sum(e["count"] for e in entries)))
        json.dump(out, open(MANIFEST, "w"), indent=1)
        print("\nwrote %s" % MANIFEST)
        return

    if args.match is not None:
        if not os.path.exists(MANIFEST):
            sys.exit("no manifest yet - run with --scan first")
        data = json.load(open(MANIFEST))
        # The zone's own DAT, through the client's file table rather than by
        # arithmetic - model file = 100 + zone is only true for the early zones.
        sys.path.insert(0, HERE)
        import zonetext
        table = zonetext.FileTable(zonetext.INSTALL)
        zone_dat = table.path(args.match + 100)
        zbox = bounds(str(zone_dat), args.scratch) if zone_dat and os.path.exists(str(zone_dat)) else None
        if not zbox:
            sys.exit("could not read zone %d's own DAT to get its extent" % args.match)
        print("zone %d spans x %.0f..%.0f  z %.0f..%.0f"
              % (args.match, zbox["lo"][0], zbox["hi"][0], zbox["lo"][2], zbox["hi"][2]))
        for tag, entries in sorted(data.items()):
            inside = [e for e in entries
                      if zbox["lo"][0] <= e["lo"][0] and e["hi"][0] <= zbox["hi"][0]
                      and zbox["lo"][2] <= e["lo"][2] and e["hi"][2] <= zbox["hi"][2]]
            if len(inside) == len(entries) and entries:
                print("  %s: %d files" % (tag, len(inside)))
                for e in inside:
                    print("     %-16s %3d placements" % (e["path"], e["count"]))


if __name__ == "__main__":
    main()
