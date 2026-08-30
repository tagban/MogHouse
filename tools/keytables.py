"""Writes the two 256-byte key tables the MZB and MMB readers need.

Both tables are lifted from the retail client, so they are not committed here.
They are, however, already present in the `ffxi-engine` submodule as C++ array
initialisers, which is where this reads them from.

    python tools/keytables.py                 # writes beside the repo, in keys/
    python tools/keytables.py --out somewhere

Then point the renderer at them:

    MOGHOUSE_FFXI_KEYTABLE=keys/mzb_key_table.bin
    MOGHOUSE_FFXI_KEYTABLE2=keys/mmb_key_table2.bin
"""

import argparse
import re
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "ffxi-engine" / "ffxi" / "dat" / "key_tables.cppm"


def read_table(text, name):
    """Pulls one 256-entry array initialiser out of the C++ source."""
    after = text.split(name, 1)
    if len(after) < 2:
        raise SystemExit(f"{name} is not in {SOURCE}")
    body = after[1].split("{", 1)[1].split("}", 1)[0]

    values = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]
    if len(values) != 256:
        raise SystemExit(f"{name} has {len(values)} entries, expected 256")
    return bytes(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=str(SOURCE.parent.parent.parent.parent / "keys"))
    args = parser.parse_args()

    if not SOURCE.exists():
        raise SystemExit(f"{SOURCE} is missing - run: git submodule update --init --recursive")

    text = SOURCE.read_text(encoding="utf-8", errors="replace")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for name, filename in (("key_table[0x100]", "mzb_key_table.bin"),
                           ("key_table2[0x100]", "mmb_key_table2.bin")):
        table = read_table(text, name)
        path = out / filename
        path.write_bytes(table)
        print(f"wrote {path} ({len(table)} bytes)")


if __name__ == "__main__":
    main()
