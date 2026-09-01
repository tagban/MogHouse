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
import re
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


def texture_names(path):
    """Every texture name in one DAT. Like the tags, these are not obfuscated."""
    try:
        raw = open(path, "rb").read()
    except OSError:
        return set()
    names, off = set(), 0
    while off + HEADER <= len(raw):
        packed = struct.unpack_from("<I", raw, off + 4)[0]
        kind, length = packed & 0x7F, ((packed >> 7) & 0x7FFFF) * 16
        if length < HEADER or off + length > len(raw):
            break
        if kind == 0x20 and off + HEADER + 17 <= len(raw):
            name = raw[off + HEADER + 1: off + HEADER + 17].decode("ascii", "replace").strip()
            if name:
                names.add(name.split()[-1])
        off += length
    return names


# How far outside a room's own placements its door is allowed to stand.
PAD = 8.0

DEFAULT_ZONEDATA = "C:/Users/Gaming/Desktop/LandSandBoat/data/zones"


def zone_doors(zone):
    """Where a zone's doors are, from the server's own data."""
    root = os.environ.get("MOGHOUSE_FFXI_ZONEDATA", DEFAULT_ZONEDATA)
    doors = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name, "npcs.yaml")
        if not os.path.exists(path):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        first = re.search(r"^  (\d{8}):", text, re.M)
        if not first or (int(first.group(1)) - 0x01000000) >> 12 != zone:
            continue
        for block in re.split(r"\n  (?=\d{8}:)", text):
            if "type: door" not in block:
                continue
            at = re.search(r"at:\s*\[\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)", block)
            if at:
                doors.append((float(at.group(1)), float(at.group(3))))
    return doors


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

        # Two things that look like they should identify a zone do not.
        # Position alone fails because every city is laid out around the origin
        # at a similar size, so any zone's box contains every family's box.
        # Textures fail because an interior carries its own art and shares
        # almost none of it with the shell around it.
        #
        # Doors work. A building's interior is behind that building's door, the
        # server knows where every door in a zone stands, and an interior's
        # placements are already in zone coordinates - so a family belongs to
        # the zone whose doors its rooms are built around.
        doors = zone_doors(args.match)
        if not doors:
            sys.exit("no doors found for zone %d - is MOGHOUSE_FFXI_ZONEDATA set?" % args.match)
        print("zone %d has %d doors" % (args.match, len(doors)))

        scored = []
        for tag, entries in data.items():
            hit = sum(1 for e in entries
                      if any(e["lo"][0] - PAD <= x <= e["hi"][0] + PAD
                             and e["lo"][2] - PAD <= z <= e["hi"][2] + PAD for x, z in doors))
            if hit:
                # Neither count nor fraction alone ranks these correctly.
                # A one-room family that matches its one room scores 100%,
                # and the largest family catches a few doors of almost any
                # city by coincidence, because cities share a coordinate
                # range. Their product wants both: many rooms matched, and
                # most of the family accounted for.
                share = hit / len(entries)
                scored.append((hit * share, hit, share, tag, entries))
        scored.sort(reverse=True)
        if not scored:
            sys.exit("no interior family sits behind this zone's doors")

        for _, hit, share, tag, entries in scored[:3]:
            print("  %s: %d of %d rooms sit on one of this zone's doors" % (tag, hit, len(entries)))

        _, hit, share, tag, entries = scored[0]
        print(chr(10) + "zone %d's interiors are %s - %d files, %d placements:"
              % (args.match, tag, len(entries), sum(e["count"] for e in entries)))
        for e in sorted(entries, key=lambda e: e["path"]):
            print("   %-18s %3d placements  x %7.1f..%-7.1f z %7.1f..%-7.1f"
                  % (e["path"], e["count"], e["lo"][0], e["hi"][0], e["lo"][2], e["hi"][2]))


if __name__ == "__main__":
    main()
