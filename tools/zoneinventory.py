"""Inventory every MZB across the whole retail install.

Reports each zone's chunk id, placement count and collision mesh count, so the
odd ones out - no collision, unusually small, several per DAT - are visible
rather than guessed at.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datscan import chunks, HEADER
from mzbdecrypt import decrypt, load_key_table_from_lotus
from mzbmesh import read_header, read_meshes

MZB_TYPE = 0x1C


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("--key-table-from-lotus", required=True)
    args = parser.parse_args()

    key_table = load_key_table_from_lotus(Path(args.key_table_from_lotus))
    root = Path(args.root)

    rows = []
    errors = []
    for path in sorted(root.rglob("*.DAT")):
        try:
            data = path.read_bytes()
        except OSError:
            continue
        for offset, name, ctype, length, _p, _c in chunks(data):
            if ctype != MZB_TYPE:
                continue
            rel = path.relative_to(root)
            try:
                buf, _ = decrypt(data[offset + HEADER: offset + length], key_table)
                header = read_header(buf)
                _, meshes = read_meshes(buf)
                rows.append((str(rel), name.decode("ascii", "replace"), header["placements"],
                             len(meshes), sum(len(m["vertices"]) for m in meshes), header["version"]))
            except Exception as exc:
                errors.append((str(rel), name, type(exc).__name__, str(exc)[:70]))

    print(f"{len(rows)} MZB chunks, {len(errors)} errors")
    print()
    zero = [r for r in rows if r[3] == 0]
    print(f"no collision geometry ({len(zero)}):")
    for row in sorted(zero, key=lambda r: r[1]):
        print(f"    {row[0]:<18} {row[1]:<8} placements={row[2]:<6} version={row[5]}")
    print()
    names = {}
    for row in rows:
        names.setdefault(row[1], []).append(row)
    print(f"{len(names)} distinct zone ids")
    if errors:
        print()
        print("errors:")
        for row in errors[:15]:
            print("   ", row)


if __name__ == "__main__":
    main()
