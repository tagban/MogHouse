"""Indexes the DATs that hold wearable equipment.

A piece of equipment is a DAT holding one or more OS2 meshes named hh_<slot>
alongside the textures they use. The texture chunk id carries the race and slot
(hm_b is a hume male body), which is what makes the files classifiable without
parsing anything.

Writes a JSON index so the mapping only has to be built once.
"""

import json
import struct
import sys
from collections import Counter, defaultdict

from filetable import FileTable

INSTALL = "C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"

# The chunk id an OS2 mesh takes, per slot.
SLOTS = {
    "1": "face",
    "m": "head",
    "b": "body",
    "h": "hands",
    "l": "legs",
    "f": "feet",
    "g": "gorget",
}

# Race prefixes as they appear on texture chunk ids. Derived by histogramming
# every prefix in the files that hold skinned meshes, not guessed from the race
# names - tarutaru are tr_ and tl_, nothing like their initials.
RACES = {
    "hm": "hume male",
    "hf": "hume female",
    "em": "elvaan male",
    "ef": "elvaan female",
    "tr": "tarutaru male",
    "tl": "tarutaru female",
    "mt": "mithra",
    "gl": "galka",
}


def chunks(data):
    off = 0
    while off + 16 <= len(data):
        packed = struct.unpack_from("<I", data, off + 4)[0]
        typ = packed & 0x7F
        length = ((packed >> 7) & 0x7FFFF) * 16
        if length < 16 or off + length > len(data):
            break
        yield typ, data[off:off + 4], off, length
        off += length


def scan(table, lo, hi):
    found = {}
    for fid in range(lo, hi):
        path = table.path(fid)
        if path is None:
            continue
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            continue

        meshes = []
        textures = []
        skeletons = []
        motions = 0
        for typ, ident, _, _ in chunks(data):
            name = ident.decode("latin1")
            if typ == 0x2A:
                meshes.append(name)
            elif typ == 0x20:
                textures.append(name)
            elif typ == 0x29:
                skeletons.append(name)
            elif typ == 0x2B:
                motions += 1

        if not meshes:
            continue

        # The race is whatever the textures agree on.
        races = Counter(t[:2] for t in textures if t[:2] in RACES)
        slots = sorted({m[3] for m in meshes if m.startswith("hh_") and m[3] in SLOTS})
        found[fid] = {
            "race": races.most_common(1)[0][0] if races else None,
            "slots": slots,
            "meshes": sorted(set(meshes)),
            "textures": sorted(set(textures)),
            "skeletons": sorted(set(skeletons)),
            "motions": motions,
        }
    return found


def main():
    lo = int(sys.argv[1]) if len(sys.argv) > 1 else 1300
    hi = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    out = sys.argv[3] if len(sys.argv) > 3 else "../ffxi-equipment.json"

    table = FileTable(INSTALL)
    found = scan(table, lo, hi)

    by_race = defaultdict(list)
    for fid, entry in found.items():
        if entry["race"]:
            by_race[entry["race"]].append(fid)

    print(f"{len(found)} files with skinned meshes in {lo}..{hi}")
    for race, ids in sorted(by_race.items()):
        ids.sort()
        print(f"  {RACES[race]:16s} {len(ids):5d} files, {ids[0]}..{ids[-1]}")

    whole = [f for f, e in found.items() if len(e["slots"]) >= 5]
    print(f"{len(whole)} files carrying five or more slots - a whole outfit in one DAT")

    with open(out, "w", encoding="utf-8") as f:
        json.dump({"range": [lo, hi], "files": found}, f)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
