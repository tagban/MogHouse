#!/usr/bin/env python3
"""Read a zone's terrain surfaces out of LandSandBoat's .ximesh files.

    python tools/ximesh.py Windurst_Waters
    python tools/ximesh.py Windurst_Waters --water     # just the water triangles

Water in FFXI is not a model you can recognise by name, and it is not the MZB's
per-cell height field either - both of those were tried and both were wrong.
It is a **material on each collision triangle**. LandSandBoat's mesh reader
keeps `material:4` and `barrier:1` per triangle
(src/map/ximesh/ximesh_structs.h) and its TerrainType puts ShallowWater at 8
and DeepWater at 9, which is what `!pos` prints as "Terrain: Deep Water".

The .ximesh files ship with the server and are zlib streams of:

    [XimeshHeader]                 gridW, gridH, blockOff, placeOff, counts
    [u32 x cellCount]              cell offset table
    Block:  [u16 vertexCount][u16 triangleCount][u16 barrier][u16 pad]
            [float x vertexCount x 3]      local space
            [u16 x triangleCount x 3]      indices, 4-byte aligned
            [u8 x triangleCount]           material:4, barrier:1
    Placement: [u32 flags][float x 12]     3x3 rotation + translation

Checked against four zones: Windurst Waters is 8.0% water, Woods 0.8%, Bastok
Markets 1.7%, Southern San d'Oria 0.1% - and the rest comes out as Wood, Stone,
Grass and Path in the proportions a city should have.
"""
import argparse
import collections
import os
import struct
import zlib

DEFAULT_ROOT = "C:/Users/Gaming/Desktop/LandSandBoat/ximeshes"

TERRAIN = {0: "Object", 1: "Path", 2: "Grass", 3: "Sand", 4: "Snow", 5: "Stone",
           6: "Metal", 7: "Wood", 8: "ShallowWater", 9: "DeepWater", 10: "Unknown"}

SHALLOW_WATER = 8
DEEP_WATER = 9


def blocks(data):
    """Every block's vertices, indices and per-triangle material."""
    _, _, block_offset, _, block_count, _, _ = struct.unpack_from("<HHIIHHI", data, 0)
    offset = block_offset
    for _ in range(block_count):
        if offset + 8 > len(data):
            return
        vertex_count, triangle_count, barrier, _ = struct.unpack_from("<HHHH", data, offset)
        at = offset + 8
        vertices = struct.unpack_from("<%df" % (vertex_count * 3), data, at)
        at += vertex_count * 12
        indices = struct.unpack_from("<%dH" % (triangle_count * 3), data, at)
        at += triangle_count * 6
        at = (at + 3) & ~3                       # indices are padded to four bytes
        materials = data[at:at + triangle_count]
        yield vertices, indices, materials, barrier
        offset = (at + triangle_count + 3) & ~3


def load(zone, root):
    path = os.path.join(root, zone + ".ximesh")
    if not os.path.exists(path):
        raise SystemExit("no ximesh for %s at %s" % (zone, path))
    return zlib.decompress(open(path, "rb").read())


def cells(data):
    """Every (block, placement) pair the grid references, each once.

    A block is geometry in its own space; a placement is where that geometry
    sits. The same pair is named by every cell it overlaps, so they are drawn
    once and remembered.
    """
    grid_w, grid_h, _, _, _, _, _ = struct.unpack_from("<HHIIHHI", data, 0)
    seen = set()
    for index in range(grid_w * grid_h):
        offset = struct.unpack_from("<I", data, 20 + index * 4)[0]
        if not offset:
            continue
        _, entry_count = struct.unpack_from("<IH", data, offset)
        for entry in range(entry_count):
            pair = struct.unpack_from("<II", data, offset + 6 + entry * 8)
            if pair not in seen:
                seen.add(pair)
                yield pair


def one_block(data, offset):
    vertex_count, triangle_count, _, _ = struct.unpack_from("<HHHH", data, offset)
    at = offset + 8
    vertices = struct.unpack_from("<%df" % (vertex_count * 3), data, at)
    at += vertex_count * 12
    indices = struct.unpack_from("<%dH" % (triangle_count * 3), data, at)
    at += triangle_count * 6
    at = (at + 3) & ~3
    return vertices, indices, data[at:at + triangle_count]


def water_triangles(data):
    """Every water triangle, in world space."""
    for block_offset, placement_offset in cells(data):
        vertices, indices, materials = one_block(data, block_offset)
        m = struct.unpack_from("<12f", data, placement_offset + 4)
        for triangle in range(len(materials)):
            if (materials[triangle] & 0x0F) not in (SHALLOW_WATER, DEEP_WATER):
                continue
            corners = []
            for corner in range(3):
                v = indices[triangle * 3 + corner]
                x, y, z = vertices[v * 3], vertices[v * 3 + 1], vertices[v * 3 + 2]
                corners.append((m[0] * x + m[3] * y + m[6] * z + m[9],
                                m[1] * x + m[4] * y + m[7] * z + m[10],
                                m[2] * x + m[5] * y + m[8] * z + m[11]))
            yield corners


def emit(zone, data, path):
    """Write the water as a flat float32 list the renderer can read straight in.

    Nine floats a triangle, already in the renderer's own frame - the world is
    (x, -y, -z) relative to the DATs, and doing the turn here means the C++
    side has a file it can upload rather than a format to understand.
    """
    triangles = list(water_triangles(data))
    with open(path, "wb") as out:
        out.write(b"MHWA")
        out.write(struct.pack("<I", len(triangles)))
        for corners in triangles:
            for x, y, z in corners:
                out.write(struct.pack("<3f", x, -y, -z))
    return len(triangles)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zone", help="zone name as the file is called, e.g. Windurst_Waters")
    ap.add_argument("--water", action="store_true", help="list the water triangles' extent")
    ap.add_argument("--emit", metavar="DIR", help="write <zone>.water for the renderer")
    ap.add_argument("--root", default=os.environ.get("MOGHOUSE_FFXI_XIMESHES", DEFAULT_ROOT))
    args = ap.parse_args()

    data = load(args.zone, args.root)

    if args.emit:
        os.makedirs(args.emit, exist_ok=True)
        out = os.path.join(args.emit, args.zone + ".water")
        print("%s: %d water triangles -> %s" % (args.zone, emit(args.zone, data, out), out))
        return

    tally = collections.Counter()
    water_points = []
    for vertices, indices, materials, _ in blocks(data):
        for triangle in range(len(materials)):
            material = materials[triangle] & 0x0F
            tally[material] += 1
            if material in (SHALLOW_WATER, DEEP_WATER) and args.water:
                for corner in range(3):
                    index = indices[triangle * 3 + corner]
                    if index * 3 + 2 < len(vertices):
                        water_points.append(vertices[index * 3: index * 3 + 3])

    total = sum(tally.values())
    print("%s: %d triangles" % (args.zone, total))
    for material, count in tally.most_common():
        print("   %-13s %6d  %5.1f%%" % (TERRAIN.get(material, material), count, 100.0 * count / max(total, 1)))

    if args.water and water_points:
        xs = [p[0] for p in water_points]
        ys = [p[1] for p in water_points]
        zs = [p[2] for p in water_points]
        print("\n%d water corners, in block-local space:" % len(water_points))
        print("   x %.1f..%.1f   y %.1f..%.1f   z %.1f..%.1f"
              % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
        print("   (local to each block - the placement transform still has to be applied)")


if __name__ == "__main__":
    main()
