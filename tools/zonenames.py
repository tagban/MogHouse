"""Work out which zone a DAT holds, by name rather than by guessing.

MZB chunk ids are four characters and look like abbreviations - f_ro, d_gh,
la_t. LandSandBoat knows zone ids and names. What connects them is that
**MZB file id = zone id + 100**, derived here rather than assumed.

How it was derived matters, because the obvious way gets it wrong. Scoring
candidate offsets on "does this land on a zone id LandSandBoat knows" picks 289
with 285 of 441 matching, which looks convincing and is nonsense - it pairs
`ship` with Monarch Linn. Almost any offset lands inside a 300-wide range, so
that test measures nothing.

Scoring on whether the chunk id actually reads as an abbreviation of the zone
name picks 100, with pairings that are unarguable: f_ro to West and East
Ronfaure, f_gu to North and South Gustaberg, d_gi to Giddeus, f_qu to Qufim
Island.

**The offset does not hold everywhere.** It is strongly evidenced for the
original-era zones - the whole 200-260 block pairs correctly - but only 35 of
143 matched chunks have an id that agrees with the name it lands on, and some
disagreements are plainly wrong: f_el is Elshimo, not Abyssea-Tahrongi. Expansion
content was added to the DATs later and does not follow the same run.

So `name_agrees` in the output is the field that matters. A row without it is a
candidate, not an answer, and this tool deliberately reports both rather than
hiding the difference behind a single number.

The prefixes look like classes - f_ for field, d_ for dungeon, s_ for the ferry
zones - but that is an observation, not something relied on here.

LandSandBoat is GPL-3.0, so nothing from it is vendored. This reads the user's
own checkout, the same way the compression tables and the MZB key table are
handled.
"""

import argparse
import json
import re
from pathlib import Path

ZONE_ID_OFFSET = 100


def load_lsb_zones(sql_path):
    text = Path(sql_path).read_text(encoding="utf-8", errors="replace")
    pattern = r"INSERT INTO `zone_settings` VALUES \((\d+),'[^']*',\d+,'([^']*)'\)"
    return {int(m.group(1)): m.group(2) for m in re.finditer(pattern, text)}


def looks_like(chunk, zone_name):
    """Whether a chunk id reads as an abbreviation of a zone name."""
    stem = chunk[2:] if len(chunk) > 2 and chunk[1] == "_" else chunk
    stem = stem.strip("_0123456789").lower()
    if len(stem) < 2:
        return False
    return any(word.startswith(stem) for word in re.split(r"[_\-\[\] ]+", zone_name.lower()) if word)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("index", help="ffxi-index.json from buildindex.py")
    parser.add_argument("--lsb", required=True, help="path to LandSandBoat's sql/zone_settings.sql")
    parser.add_argument("--out")
    args = parser.parse_args()

    index = json.loads(Path(args.index).read_text(encoding="utf-8"))
    lsb = load_lsb_zones(args.lsb)

    named = {}
    confident = 0
    for chunk, file_ids in index["zones"].items():
        for file_id in file_ids:
            zone_id = file_id - ZONE_ID_OFFSET
            name = lsb.get(zone_id)
            if not name:
                continue
            agrees = looks_like(chunk, name)
            confident += agrees
            named[file_id] = {"chunk": chunk, "zone_id": zone_id, "name": name, "name_agrees": agrees}

    print(f"{len(named)} MZB chunks matched to zone ids, {confident} where the chunk id agrees with the name")
    for file_id in sorted(named)[:20]:
        row = named[file_id]
        mark = "*" if row["name_agrees"] else " "
        print(f"  {mark} file {file_id:6}  zone {row['zone_id']:4}  {row['chunk']:6}  {row['name']}")

    if args.out:
        Path(args.out).write_text(json.dumps(named, indent=1), encoding="utf-8")
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
