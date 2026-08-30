"""Derives where a race's equipment models live in the file table.

The layout turns out to be regular: each playable race gets one contiguous
block of file ids, and inside it the slots follow in a fixed order at fixed
offsets.

    +0     skeleton
    +8     face       32 entries
    +40    head      256
    +296   body      256
    +552   hands     256
    +808   legs      256
    +1064  feet      256

A model id indexes directly into its slot's window, so file id = race base +
slot offset + model id. Ids with no file are simply gear that race never wears.

Nothing here is assumed. The race bases are found by scoring every candidate
against the equipment index: how many of the files that actually hold, say, a
hume male body mesh land inside the window this layout predicts for bodies. The
right base scores nearly everything and the wrong one scores nothing, so there
is no judgement involved in reading the result.
"""

import json
import sys
from collections import defaultdict

from chunknames import INSTALL, ids
from filetable import FileTable

# Offset from the race base, and how many model ids the window holds. The
# order is the order the slots appear in the file table.
SLOT_LAYOUT = [
    ("h", 8, 32),      # the character's own head: face and hair
    ("m", 40, 256),    # headgear
    ("b", 296, 256),   # body
    ("g", 552, 256),   # hands
    ("l", 808, 256),   # legs
    ("f", 1064, 256),  # feet
]

RACE_ORDER = ["hm", "hf", "em", "ef", "tr", "tl", "mt", "gl"]


def score(base, ids_by_slot):
    """How many files of each slot land in the window this base predicts."""
    hits = 0
    misplaced = 0
    for slot, offset, count in SLOT_LAYOUT:
        low = base + offset
        high = low + count
        for other, ids in ids_by_slot.items():
            inside = sum(1 for i in ids if low <= i < high)
            if other == slot:
                hits += inside
            else:
                misplaced += inside
    return hits, misplaced


def main():
    index = sys.argv[1] if len(sys.argv) > 1 else "../ffxi-equipment.json"
    out = sys.argv[2] if len(sys.argv) > 2 else "../ffxi-pcmodels.json"

    files = json.load(open(index, encoding="utf-8"))["files"]
    table = FileTable(INSTALL)

    # Only files holding exactly one slot are equipment; anything with several
    # is a whole NPC and sits outside these blocks.
    per_race = defaultdict(lambda: defaultdict(list))
    for fid, entry in files.items():
        if entry["race"] and len(entry["slots"]) == 1:
            per_race[entry["race"]][entry["slots"][0]].append(int(fid))

    result = {}
    for race in RACE_ORDER:
        ids_by_slot = per_race.get(race)
        if not ids_by_slot:
            continue

        # Candidates: every file id that could be a base, taken from the data
        # rather than swept blindly. A base sits 296 below some body file.
        candidates = set()
        for slot, offset, _ in SLOT_LAYOUT:
            for i in ids_by_slot.get(slot, []):
                candidates.add(i - offset)

        # Every block of gear repeats the same shape, so a later expansion
        # block scores as well as the original and the score alone cannot tell
        # them apart. What can: the race base is the file holding that race's
        # skeleton, and the expansion blocks hold no skeleton at all.
        best = None
        for base in sorted(candidates):
            path = table.path(base) if 0 <= base < len(table._vtable) else None
            if path is None or not any(typ == 0x29 for typ, _ in ids(path)):
                continue
            hits, misplaced = score(base, ids_by_slot)
            if best is None or (hits - misplaced) > (best[1] - best[2]):
                best = (base, hits, misplaced)

        if best is None:
            print(f"{race}: no candidate base holds a skeleton")
            continue
        base, hits, misplaced = best
        total = sum(len(v) for v in ids_by_slot.values())
        print(f"{race}: base {base}   {hits} of {total} files placed, {misplaced} in the wrong window")
        result[race] = base

    with open(out, "w", encoding="utf-8") as f:
        json.dump({"race_base": result, "slots": [[s, o, c] for s, o, c in SLOT_LAYOUT]}, f, indent=1)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
