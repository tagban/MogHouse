"""Decrypt an MZB chunk and read its placement table.

The scheme, verified against the retail DATs:

  * Byte 3 of the payload is a version. 0x1B and above are encrypted.
  * Word 0, low 24 bits, is the encrypted length. Word 1, low 24 bits, is the
    number of placement entries.
  * The key starts as key_table[payload[7] ^ 0xFF] and walks forward from
    offset 8 in variable-sized runs: run = ((key >> 4) & 7) + 16 bytes, XORed
    with 0xFF only when the key is odd, then key += ++counter.
  * Placement entries begin at offset 32 and are 0x64 bytes each. Their 16-byte
    model id is separately XORed with 0x55.

key_table is 256 bytes lifted from the retail client, so it is not in this
repository. Point --key-table at a file that has it; --key-table-from-lotus
scrapes it out of the lotus-ffxi source for local checking.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mzbdump import extract

ENTRY_SIZE = 0x64


def load_key_table_from_lotus(path):
    text = path.read_text(encoding="utf-8")
    body = text.split("key_table[0x100]", 1)[1].split("{", 1)[1].split("}", 1)[0]
    values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if len(values) != 256:
        raise SystemExit(f"expected 256 key bytes, found {len(values)}")
    return bytes(values)


def decrypt(payload, key_table):
    buffer = bytearray(payload)
    if buffer[3] < 0x1B:
        return buffer, False

    length = struct.unpack_from("<I", buffer, 0)[0] & 0x00FFFFFF
    if length > len(buffer):
        raise SystemExit(f"declared length {length} exceeds payload {len(buffer)}")

    key = key_table[buffer[7] ^ 0xFF]
    counter = 0
    pos = 8
    while pos < length:
        run = ((key >> 4) & 7) + 16
        if (key & 1) and pos + run < length:
            for i in range(run):
                buffer[pos + i] ^= 0xFF
        counter += 1
        key = (key + counter) & 0xFFFFFFFF
        pos += run

    count = struct.unpack_from("<I", buffer, 4)[0] & 0x00FFFFFF
    for i in range(count):
        base = 32 + i * ENTRY_SIZE
        for j in range(16):
            buffer[base + j] ^= 0x55
    return buffer, True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dat")
    parser.add_argument("--key-table-from-lotus")
    parser.add_argument("--entries", type=int, default=8)
    args = parser.parse_args()

    key_table = load_key_table_from_lotus(Path(args.key_table_from_lotus))
    name, payload = extract(Path(args.dat))
    if payload is None:
        raise SystemExit("no MZB chunk in that file")

    buffer, was_encrypted = decrypt(payload, key_table)
    count = struct.unpack_from("<I", buffer, 4)[0] & 0x00FFFFFF
    print(f"chunk {name!r}  encrypted={was_encrypted}  placements={count}")
    print()
    print("  model id          translate                    rotate                 scale")
    for i in range(min(count, args.entries)):
        base = 32 + i * ENTRY_SIZE
        raw_id = bytes(buffer[base:base + 16])
        model = raw_id.split(b"\0")[0].decode("ascii", "replace")
        tx, ty, tz, rx, ry, rz, sx, sy, sz = struct.unpack_from("<9f", buffer, base + 16)
        print(f"  {model:<16}  {tx:9.2f} {ty:8.2f} {tz:8.2f}   {rx:6.2f} {ry:6.2f} {rz:6.2f}   {sx:5.2f} {sy:5.2f} {sz:5.2f}")


if __name__ == "__main__":
    main()
