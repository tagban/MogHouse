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


# How wide a cell is when grouping water into pools, and how far up the sorted
# bed heights the waterline is taken from. A percentile rather than the very
# highest point, so one stray triangle on a bank does not raise a whole canal.
POOL_CELL = 4.0
WATERLINE_PERCENTILE = 0.05

# How far apart two neighbouring cells' beds may be and still count as the same
# body of water.
#
# Without this, adjacency alone decides, and adjacency is not enough: in Bastok
# Markets the moat around the city runs beside a small canal cut into the
# streets, and every water triangle in the zone - 12,467 of 12,697 - flooded
# into one pool spanning three bed heights. One waterline was then chosen for
# all of it and the canal was drawn at the moat's level, several units under
# the street it belongs to.
#
# Water that is ten units lower than the water beside it is a different body of
# water, whatever the map does. At 2.0 Bastok separates into seven pools and
# the canal takes its own line.
POOL_JOIN_HEIGHT = 2.0

# How far around a cell the waterline is measured, in cells.
#
# Water is level only if it is standing. Most of FFXI's is not: canals and
# rivers run downhill, and Bastok's central canal drops two units over its
# length. One height for a whole pool draws that flat, which leaves the upper
# end's surface *below its own bed* - the water is still there, buried under
# the channel it belongs to, which is exactly how it reads as missing.
#
# So the line is measured near each cell rather than across the whole pool. A
# standing pool gives the same answer everywhere and stays level; a channel
# gives an answer that follows its grade. Measured on Bastok Markets: the canal
# comes out sloping 1.68 down to 0.03 while the moat beside it holds -8.00
# across all 5,163 of its cells.
WATERLINE_RADIUS = 6

# How far above its own bed a surface sits when the shape cannot say.
#
# A channel with a flat stone floor has no bank inside its water for the line
# to find, so the best the shape can offer is the floor itself - and a surface
# laid exactly on its own bed is coplanar with the stone, which z-fights and
# reads as patchy water barely coming through the ground.
#
# This is a judgement call and should be read as one: the real level is not in
# any data we have. The MZB's per-cell height says 2.00 for Bastok's central
# basin, whose floor is also 2.00, and every water model in that zone sits at
# world -8.0 or 0.0 - at or below the floor. Retail plainly fills it well
# higher. Against a side-by-side of that basin, the water reaches roughly a
# third of the way up a wall about four units tall.
WATERLINE_MIN_DEPTH = 1.0

# A sea is not a bed. Port Bastok's harbour arrives as 6,144 water triangles
# at exactly one height - half a unit under the quay - which is the surface
# the mesh gives players to stand at the edge of, not a floor with water on
# top. Lifting that by WATERLINE_MIN_DEPTH put the harbour over the quay and
# a Galka knee-deep on the dock. A pool this large and this flat is the
# surface itself, raised only enough not to z-fight with its own plane.
FLAT_SEA_TRIANGLES = 500
FLAT_SEA_TOLERANCE = 0.25
FLAT_SEA_LIFT = 0.05

# How far a surface will reach across a hole in its own bed, in cells.
#
# The server's mesh is the terrain the server cares about, which is what a
# player can walk on. Where a bridge or a tunnel roof is the walkable surface,
# the canal floor beneath it is not in the mesh at all - so a canal arrives
# here in pieces, with a gap wherever something crosses over it. Bastok's has
# a four-cell hole under one bridge and another under a tunnel.
#
# The water is not misplaced in those gaps, it is missing, so it is bridged: a
# cell with no bed of its own is filled if the same pool continues on both
# sides of it within this reach. Short on purpose - it is meant to cross a
# bridge, not to flood a courtyard that happens to sit between two canals.
GAP_SPAN = 4


def _pools(triangles):
    """The cell grid, its pools, and a local waterline for each cell.

    Shared by the two things that need it: the surface for a triangle that has
    a bed, and the surface for a cell that has none.
    """
    cells = collections.defaultdict(list)
    owner = {}
    for index, corners in enumerate(triangles):
        cx = int(sum(c[0] for c in corners) / 3 // POOL_CELL)
        cz = int(sum(c[2] for c in corners) / 3 // POOL_CELL)
        cells[(cx, cz)].append(min(c[1] for c in corners))
        owner[index] = (cx, cz)

    # A cell's own level is its highest bed point, which in DAT space is its
    # smallest y. Two cells only join a pool if theirs are close.
    level = {cell: min(tops) for cell, tops in cells.items()}

    pool_of = {}
    seen = set()
    for start in cells:
        if start in seen:
            continue
        stack, pool = [start], []
        seen.add(start)
        while stack:
            cx, cz = stack.pop()
            pool.append((cx, cz))
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    near = (cx + dx, cz + dz)
                    if (near in cells and near not in seen and
                            abs(level[near] - level[(cx, cz)]) <= POOL_JOIN_HEIGHT):
                        seen.add(near)
                        stack.append(near)
        for cell in pool:
            pool_of[cell] = start

    # The local bank, as a separable minimum over each pool - along x, then
    # along z. Separable so this stays linear in the radius rather than square
    # in it; there are zones here with tens of thousands of water cells.
    def sweep(source, axis):
        out = {}
        for cell in source:
            best = source[cell]
            for step in range(-WATERLINE_RADIUS, WATERLINE_RADIUS + 1):
                near = (cell[0] + step, cell[1]) if axis == 0 else (cell[0], cell[1] + step)
                if near in source and pool_of[near] == pool_of[cell]:
                    best = min(best, source[near])
            out[cell] = best
        return out

    return cells, owner, pool_of, sweep(sweep(level, 0), 1)


def waterlines(triangles):
    """A surface height for each water triangle.

    The MZB's per-cell height is the obvious source and is not usable: Windurst
    Waters writes 0.0010 into every water cell, which is a flag rather than a
    height, and lifting to it drags surfaces below their own bed. The shape
    answers instead - water meets its bank at the highest ground it touches.

    DAT space has y increasing downward, so the highest ground is the smallest
    y, and a waterline is near the bottom of the sorted heights.

    Three things the obvious version gets wrong, all found in Bastok Markets:
    pools must not merge across a height step (POOL_JOIN_HEIGHT), the line has
    to be local because most of this game's water runs rather than stands
    (WATERLINE_RADIUS), and a surface resting exactly on its own bed z-fights
    with the stone (WATERLINE_MIN_DEPTH).
    """
    _, owner, pool_of, local = _pools(triangles)

    # Which pools are a flat sea - see FLAT_SEA_TRIANGLES. A harbour pool also
    # takes in the drains and steps beside it, so the test is not "all one
    # height" but "overwhelmingly one height": the level most of the pool's
    # bed sits at, when four fifths of it sits within a hair of that level.
    beds = {}
    for i, tri in enumerate(triangles):
        beds.setdefault(pool_of[owner[i]], []).append(min(c[1] for c in tri))
    sea_level = {}
    for pool, tops in beds.items():
        if len(tops) < FLAT_SEA_TRIANGLES:
            continue
        counts = {}
        for top in tops:
            key = round(top / FLAT_SEA_TOLERANCE)
            counts[key] = counts.get(key, 0) + 1
        key, count = max(counts.items(), key=lambda kv: kv[1])
        level = key * FLAT_SEA_TOLERANCE
        if sum(1 for top in tops if abs(top - level) <= FLAT_SEA_TOLERANCE) >= 0.8 * len(tops):
            sea_level[pool] = level

    # Never below the triangle's own highest corner, and never resting exactly
    # on it. Below means water buried in the channel it belongs to - invisible,
    # and the bug this whole file exists to fix. Exactly on it means coplanar
    # with the stone, which z-fights and reads as patchy.
    #
    # Subtracting raises it: DAT y increases downward.
    out = []
    for i, tri in enumerate(triangles):
        top = min(c[1] for c in tri)
        level = sea_level.get(pool_of[owner[i]])
        if level is not None and abs(top - level) <= FLAT_SEA_TOLERANCE:
            out.append(top - FLAT_SEA_LIFT)
        else:
            out.append(min(local[owner[i]], top - WATERLINE_MIN_DEPTH))
    return out


def bridged(triangles):
    """Squares of water for cells whose bed is missing under something solid.

    Yields (corners, surface) shaped like a bed triangle, so the caller writes
    them the same way.
    """
    cells, _, pool_of, local = _pools(triangles)
    if not cells:
        return

    xs = [c[0] for c in cells]
    zs = [c[1] for c in cells]
    for cx in range(min(xs), max(xs) + 1):
        for cz in range(min(zs), max(zs) + 1):
            if (cx, cz) in cells:
                continue

            found = []
            for axis in (0, 1):
                before = after = None
                for step in range(1, GAP_SPAN + 1):
                    near = (cx - step, cz) if axis == 0 else (cx, cz - step)
                    if near in cells:
                        before = near
                        break
                for step in range(1, GAP_SPAN + 1):
                    near = (cx + step, cz) if axis == 0 else (cx, cz + step)
                    if near in cells:
                        after = near
                        break
                if (before and after and pool_of[before] == pool_of[after] and
                        abs(local[before] - local[after]) <= POOL_JOIN_HEIGHT):
                    found.append((local[before] + local[after]) / 2.0)

            if not found:
                continue

            surface = sum(found) / len(found)
            x0, x1 = cx * POOL_CELL, (cx + 1) * POOL_CELL
            z0, z1 = cz * POOL_CELL, (cz + 1) * POOL_CELL
            yield [(x0, surface, z0), (x1, surface, z0), (x1, surface, z1)], surface
            yield [(x0, surface, z0), (x1, surface, z1), (x0, surface, z1)], surface


def emit(zone, data, path):
    """Write the water as a flat float32 list the renderer can read straight in.

    Nine floats a triangle, already in the renderer's own frame - the world is
    (x, -y, -z) relative to the DATs, and doing the turn here means the C++
    side has a file it can upload rather than a format to understand.
    """
    triangles = list(water_triangles(data))
    surfaces = waterlines(triangles)
    spans = list(bridged(triangles))

    with open(path, "wb") as out:
        out.write(b"MHWA")
        out.write(struct.pack("<I", len(triangles) + len(spans)))
        for corners, surface in zip(triangles, surfaces):
            # Flat, at its pool's waterline. These triangles are the bed, and a
            # water surface is level where a bed is not.
            for x, _, z in corners:
                out.write(struct.pack("<3f", x, -surface, -z))
        for corners, surface in spans:
            # A square carrying the water under a bridge, where the server's
            # mesh has no bed because the bridge is what you walk on.
            for x, _, z in corners:
                out.write(struct.pack("<3f", x, -surface, -z))
    return len(triangles) + len(spans)


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
