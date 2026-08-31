"""Finds *where* the drawn world and the walked world disagree.

score_align answers "is the map the right way round" with a single number for
the whole zone, which is the right question for a global flip and useless for
anything local. A bridge placed with the wrong rotation is one model among a
thousand: it moves the score by a fraction of a percent and hides inside a
99.8% pass.

This scores the same two images per block instead, and reports the blocks
where the collision says "walkable" and the render drew nothing - in world
coordinates, so the viewer can be pointed straight at them.

    python tools/alignhotspots.py <scratch-dir> <map.bmp> <centreX> <centreZ> <units>

The centre and extent come from the renderer's own line:

    map: baked 2048x2048 covering 817 units, centred on -116 7
"""
import struct
import sys
from pathlib import Path

BLOCK = 64          # texels a side
MIN_WALKABLE = 200  # ignore blocks with almost nothing in them


def read_bmp(path):
    raw = path.read_bytes()
    offset = struct.unpack_from("<I", raw, 10)[0]
    width = struct.unpack_from("<i", raw, 18)[0]
    height = struct.unpack_from("<i", raw, 22)[0]
    row_bytes = width * 3
    padding = (4 - (row_bytes % 4)) % 4
    rows = []
    for y in range(height):
        src = offset + (height - 1 - y) * (row_bytes + padding)
        rows.append(raw[src:src + row_bytes])
    return width, height, rows


def read_pgm(path):
    raw = path.read_bytes()
    fields = []
    i = 0
    while len(fields) < 4:
        while raw[i:i + 1].isspace():
            i += 1
        start = i
        while not raw[i:i + 1].isspace():
            i += 1
        fields.append(raw[start:i])
    i += 1
    width, height = int(fields[1]), int(fields[2])
    return width, height, raw[i:i + width * height]


def main():
    map_path = Path(sys.argv[1])
    centre_x = float(sys.argv[2])
    centre_z = float(sys.argv[3])
    units = float(sys.argv[4])

    width, height, rows = read_bmp(map_path)
    mask_w, mask_h, mask = read_pgm(Path(str(map_path) + ".mask.pgm"))

    # World units per texel, and the world position of texel (0, 0). The bake
    # looks straight down with +z up the screen, so screen y runs opposite z.
    scale = units / width
    left = centre_x - units * 0.5
    top = centre_z + units * 0.5

    blocks = []
    for by in range(0, height, BLOCK):
        for bx in range(0, width, BLOCK):
            walkable = 0
            missing = 0
            for y in range(by, min(by + BLOCK, height), 2):
                row = rows[y]
                for x in range(bx, min(bx + BLOCK, width), 2):
                    if not mask[y * mask_w + x]:
                        continue
                    walkable += 1
                    c = x * 3
                    if row[c] + row[c + 1] + row[c + 2] <= 24:
                        missing += 1
            if walkable >= MIN_WALKABLE:
                blocks.append((missing / walkable, walkable, bx, by))

    blocks.sort(reverse=True)
    print(f"{len(blocks)} blocks of {BLOCK}x{BLOCK} with enough walkable area")
    print()
    print("  worst disagreement - collision says floor, nothing was drawn")
    print(f"  {'miss':>6}  {'texels':>7}   world x, z")
    for share, walkable, bx, by in blocks[:12]:
        wx = left + (bx + BLOCK * 0.5) * scale
        wz = top - (by + BLOCK * 0.5) * scale
        print(f"  {share * 100:5.1f}%  {walkable:7d}   {wx:8.1f} {wz:8.1f}")

    total_walkable = sum(b[1] for b in blocks)
    total_missing = sum(b[0] * b[1] for b in blocks)
    print()
    print(f"  overall {100 * (1 - total_missing / total_walkable):.1f}% of walkable area has terrain drawn on it")


if __name__ == "__main__":
    main()
