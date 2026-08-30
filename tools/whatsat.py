"""What does the data say should be at a given spot in a zone?

Turns "there is black over there" into an answerable question. Give it a zone
DAT and a world x/z, and it reports the placements near that point, which model
each names, whether that model exists and parses, and what textures it uses.

Coordinates are FFXI's own, as the renderer prints them with `p` - except that
the renderer flips Y to point up, so a Y from the renderer is the negation of
the Y in the file. X and Z are unchanged, and are what matter here.
"""

import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from datscan import chunks, HEADER
from mzbdecrypt import decrypt, load_key_table_from_lotus
from mzbmesh import read_header
from mmbdecrypt import decode_mmb

MZB, MMB, TEXTURE = 0x1C, 0x2E, 0x20


def parse_model(buf):
    """Walks an MMB far enough to say whether it is readable and what it uses.

    Mirrors renderer/ffxi/mmb.cpp. Returns (meshes, textures) or raises.
    """
    layout = buf[4]
    stride = 48 if layout == 2 else 36
    pieces = struct.unpack_from("<i", buf, 32)[0]
    block_offset = struct.unpack_from("<I", buf, 60)[0]

    offsets = []
    if block_offset == 0:
        offsets = [64]
    else:
        cursor = 64
        while cursor + 4 <= block_offset and len(offsets) < pieces:
            value = struct.unpack_from("<I", buf, cursor)[0]
            if value:
                offsets.append(value)
            cursor += 4
        if not offsets:
            offsets = [block_offset]

    meshes = 0
    textures = set()
    for offset in offsets:
        count = struct.unpack_from("<i", buf, offset)[0]
        offset += 32
        for _ in range(count):
            if offset + 20 > len(buf):
                raise ValueError("mesh header past end")
            textures.add(bytes(buf[offset:offset + 16]).decode("ascii", "replace").rstrip(" \0"))
            vertices = struct.unpack_from("<H", buf, offset + 16)[0]
            offset += 20
            if offset + vertices * stride > len(buf):
                raise ValueError("vertex data past end")
            offset += vertices * stride
            indices = struct.unpack_from("<H", buf, offset)[0]
            offset += 4
            if offset + indices * 2 > len(buf):
                raise ValueError("index data past end")
            offset += indices * 2
            meshes += 1
    return meshes, textures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dat")
    parser.add_argument("x", type=float)
    parser.add_argument("z", type=float)
    parser.add_argument("--radius", type=float, default=40.0)
    parser.add_argument("--key-table-from-lotus", required=True)
    args = parser.parse_args()

    keys = load_key_table_from_lotus(Path(args.key_table_from_lotus))
    data = Path(args.dat).read_bytes()

    models = {}
    for offset, name, ctype, length, _p, _c in chunks(data):
        if ctype != MMB:
            continue
        try:
            buf = bytes(decode_mmb(data[offset + HEADER: offset + length]))
        except Exception as exc:
            continue
        model = buf[16:32].decode("ascii", "replace").rstrip(" \0")
        try:
            models[model] = parse_model(buf)
        except Exception as exc:
            models[model] = exc

    for offset, name, ctype, length, _p, _c in chunks(data):
        if ctype != MZB:
            continue
        buf, _ = decrypt(data[offset + HEADER: offset + length], keys)
        header = read_header(buf)

        near = []
        for i in range(header["placements"]):
            base = 32 + i * 0x64
            model = bytes(buf[base:base + 16]).split(b"\0")[0].decode("ascii", "replace").rstrip()
            tx, ty, tz = struct.unpack_from("<3f", buf, base + 16)
            distance = math.hypot(tx - args.x, tz - args.z)
            if distance <= args.radius:
                near.append((distance, model, tx, ty, tz))

        near.sort()
        print(f"{len(near)} placements within {args.radius:.0f} units of ({args.x:.1f}, {args.z:.1f})")
        print()
        broken = 0
        for distance, model, tx, ty, tz in near[:25]:
            state = models.get(model)
            if state is None:
                note = "MODEL NOT IN THIS DAT"
            elif isinstance(state, Exception):
                note = f"FAILS TO PARSE: {state}"
                broken += 1
            else:
                meshes, textures = state
                note = f"{meshes} meshes, textures: {', '.join(sorted(textures))}"
            print(f"  {distance:6.1f}  {model:<18} at {tx:8.1f} {ty:7.1f} {tz:8.1f}")
            print(f"          {note}")
        if broken:
            print()
            print(f"{broken} of the nearest placements reference a model that does not parse")
        break


if __name__ == "__main__":
    main()
