"""Walk every installed file id and record what kind of content it holds.

The retail install has no manifest - a file id resolves to a path, and what is
inside is only discoverable by opening it. This builds the index the client has
and we do not: which ids hold zones, models, skeletons and so on.

Writes JSON so the result can be diffed between game versions, which is also how
a transcode cache would know what to rebuild.
"""

import argparse
import json
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datscan import chunks
from filetable import FileTable

MZB = 0x1C
SK2 = 0x29
MMB = 0x2E


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("install")
    parser.add_argument("--out", default="ffxi-index.json")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    table = FileTable(args.install)
    ids = list(table.present())
    if args.limit:
        ids = ids[: args.limit]

    zones = {}
    by_type = Counter()
    contents = {}
    unreadable = 0
    started = time.perf_counter()

    for n, file_id in enumerate(ids):
        path = table.path(file_id)
        try:
            data = path.read_bytes()
        except OSError:
            unreadable += 1
            continue

        types = Counter()
        for offset, name, ctype, length, _p, _c in chunks(data):
            types[ctype] += 1
            if ctype == MZB:
                # Several ids can hold the same zone id - the ferry routes reuse
                # theirs - so keep every one rather than the last.
                zones.setdefault(name.decode("ascii", "replace"), []).append(file_id)

        if types:
            contents[file_id] = {f"0x{t:02X}": c for t, c in sorted(types.items())}
            by_type.update(types)

        if n % 5000 == 0 and n:
            rate = n / (time.perf_counter() - started)
            print(f"  {n}/{len(ids)}  {rate:.0f} files/s", flush=True)

    elapsed = time.perf_counter() - started
    index = {
        "install": str(args.install),
        "file_ids_total": len(table),
        "file_ids_installed": len(ids),
        "unreadable": unreadable,
        "zones": {k: sorted(v) for k, v in sorted(zones.items())},
        "chunk_type_counts": {f"0x{t:02X}": c for t, c in sorted(by_type.items())},
        "contents": contents,
    }
    Path(args.out).write_text(json.dumps(index, indent=1), encoding="utf-8")

    print()
    print(f"{len(ids)} files in {elapsed:.0f}s, {unreadable} unreadable")
    print(f"{len(zones)} distinct zone ids across {sum(len(v) for v in zones.values())} MZB chunks")
    print(f"models(0x2E)={by_type[MMB]}  skeletons(0x29)={by_type[SK2]}  zones(0x1C)={by_type[MZB]}")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
