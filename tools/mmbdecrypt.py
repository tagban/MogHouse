"""Decrypt MMB model chunks and read their names.

MMB is obfuscated in two stages, both keyed off byte 5 of the payload:

  1. If byte 3 >= 5, every byte from offset 8 to the declared length is XORed
     with a rolling key. The key advances twice per byte, and the shift applied
     comes from the key *after* the first advance - getting that order wrong
     produces plausible-looking rubbish rather than an obvious failure.
  2. If bytes 6 and 7 are both 0xFF, the body is additionally split in half and
     8-byte blocks are swapped between the halves wherever the second key is
     odd.

Verified by the names coming out readable: lake_1_m, _sal_w01_h, _sal_w01_m.
Random bytes do not decrypt into a zone's asset list.

See docs/mmb-format.md for the header layout.
"""

import struct, sys
from pathlib import Path
sys.path.insert(0, "tools")
from datscan import chunks, HEADER
from mzbdecrypt import load_key_table_from_lotus
import re

def load_table(path, which):
    text = Path(path).read_text(encoding="utf-8")
    body = text.split(which, 1)[1].split("{", 1)[1].split("}", 1)[0]
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    assert len(vals) == 256, len(vals)
    return bytes(vals)

KT = "C:/Users/Gaming/Desktop/MogHouse/ffxi-engine/ffxi/dat/key_tables.cppm"
key_table = load_table(KT, "key_table[0x100]")
key_table2 = load_table(KT, "key_table2[0x100]")

def decode_mmb(payload):
    buf = bytearray(payload)
    length = struct.unpack_from("<I", buf, 0)[0] & 0x00FFFFFF
    length = min(length, len(buf))

    if buf[3] >= 5:
        key = key_table[buf[5] ^ 0xF0]
        count = 0
        for pos in range(8, length):
            x = ((key & 0xFF) << 8) | (key & 0xFF)
            count += 1
            key = (key + count) & 0xFFFFFFFF
            buf[pos] ^= (x >> (key & 7)) & 0xFF
            count += 1
            key = (key + count) & 0xFFFFFFFF

    if buf[6] == 0xFF and buf[7] == 0xFF:
        key1 = buf[5] ^ 0xF0
        key2 = key_table2[key1]
        decode_count = ((length - 8) & ~0xF) // 2
        for pos in range(0, decode_count, 8):
            if key2 & 1:
                a = 8 + pos
                b = 8 + decode_count + pos
                if b + 8 <= len(buf):
                    buf[a:a+8], buf[b:b+8] = buf[b:b+8], buf[a:a+8]
            key1 = (key1 + 9) & 0xFFFFFFFF
            key2 = (key2 + key1) & 0xFFFFFFFF
    return buf

p = Path("C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI/ROM/1/0.DAT")
data = p.read_bytes()
shown = 0
for off, name, t, length, _pa, _c in chunks(data):
    if t != 0x2E:
        continue
    buf = decode_mmb(data[off + HEADER: off + length])
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in buf[:96])
    print(f"{name!r:10} -> {text[:80]}")
    shown += 1
    if shown >= 8:
        break

print()
print("=== header bytes of one decrypted MMB ===")
for off, name, t, length, _pa, _c in chunks(data):
    if t != 0x2E:
        continue
    buf = decode_mmb(data[off + HEADER: off + length])
    for row in range(0, 80, 16):
        blk = buf[row:row+16]
        hexed = " ".join(f"{b:02x}" for b in blk)
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in blk)
        print(f"  +{row:04x}  {hexed:<48} {txt}")
    print()
    print("  first 8 bytes as u32 pair:", struct.unpack_from("<2I", buf, 0))
    break
