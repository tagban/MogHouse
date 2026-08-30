"""Pull the MZB chunk out of a zone DAT and start decoding its header.

Companion to datscan.py. Everything printed here is read straight from the file
so the format can be checked rather than assumed.
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datscan import chunks, HEADER

MZB_TYPE = 0x1C


def extract(path):
    data = path.read_bytes()
    for offset, name, ctype, length, _p, _c in chunks(data):
        if ctype == MZB_TYPE:
            return name, data[offset + HEADER:offset + length]
    return None, None


def main(path):
    name, payload = extract(path)
    if payload is None:
        print(f"{path}: no MZB chunk")
        return

    print(f"{path}")
    print(f"  chunk id   {name!r}")
    print(f"  payload    {len(payload)} bytes")
    print()
    print("  first 64 bytes:")
    for row in range(0, 64, 16):
        chunk = payload[row:row + 16]
        hexed = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"    +{row:04x}  {hexed:<48} {text}")
    print()

    # First four bytes look like a magic; the next like a packed size.
    magic = payload[:4]
    word = struct.unpack_from("<I", payload, 4)[0]
    print(f"  magic      {magic!r}")
    print(f"  word@4     {word:#010x}  low24={word & 0xFFFFFF}  high8={word >> 24:#04x}")
    print(f"  as u32[0:8]: {struct.unpack_from('<8I', payload, 0)}")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        main(Path(arg))
        print()
