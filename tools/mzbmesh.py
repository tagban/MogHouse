"""Read MZB's collision meshes and check the layout is actually right.

The header is 32 bytes:

    +0x00  u24      encrypted length, then u8 version (NOT an id - lotus's
                    struct calls this char id[4], but the decryption reads the
                    length from here, so it cannot also be a name)
    +0x04  u24      placement count, then u8 flags
    +0x08  u32      collision mesh table offset
    +0x0C  u8 x4    grid width, grid height, bucket width, bucket height
    +0x10  u32      quadtree offset
    +0x14  u32      object end offset
    +0x18  u32      short names offset
    +0x1C  i32      unidentified

A mesh table offset of 0 means the zone has no collision geometry.

At the collision table: u32 mesh count, u32 offset of the first mesh entry.
Each entry is 16 bytes:

    +0x00  u32  vertex data offset
    +0x04  u32  normal data offset
    +0x08  u32  triangle data offset
    +0x0C  u16  triangle count
    +0x0E  u16  flags

Vertices run from their offset to the normals offset as vec3, normals likewise
to the triangle offset. Triangles are four u16 each - three indices masked to
0x3FFF, so the top two bits carry something else, plus a fourth value.

Entries are walked back to back: the next starts at
triangle offset + count * 8.
"""

import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mzbdump import extract
from mzbdecrypt import decrypt, load_key_table_from_lotus

INDEX_MASK = 0x3FFF


def read_header(buf):
    word0 = struct.unpack_from("<I", buf, 0)[0]
    word1, mesh_table, = struct.unpack_from("<II", buf, 4)
    grid_w, grid_h, bucket_w, bucket_h = struct.unpack_from("<4B", buf, 12)
    quadtree, object_end, shortnames, unk5 = struct.unpack_from("<IIIi", buf, 16)
    return {
        "length": word0 & 0xFFFFFF,
        "version": word0 >> 24,
        "placements": word1 & 0xFFFFFF,
        "flags": word1 >> 24,
        "mesh_table": mesh_table,
        "grid": (grid_w, grid_h, bucket_w, bucket_h),
        "quadtree": quadtree,
        "object_end": object_end,
        "shortnames": shortnames,
        "unk5": unk5,
    }


def read_meshes(buf):
    header = read_header(buf)
    # 0 means the zone carries no collision geometry at all, not a table at
    # offset 0 - reading one there yields the length/version word and offsets in
    # the billions.
    if header["mesh_table"] == 0:
        return header, []
    count, first = struct.unpack_from("<II", buf, header["mesh_table"])
    meshes = []
    offset = first
    for _ in range(count):
        v_off, n_off, t_off = struct.unpack_from("<III", buf, offset)
        tri_count, flags = struct.unpack_from("<HH", buf, offset + 12)

        vertex_bytes = n_off - v_off
        normal_bytes = t_off - n_off
        vertices = [struct.unpack_from("<3f", buf, v_off + i * 12) for i in range(vertex_bytes // 12)]
        normals = [struct.unpack_from("<3f", buf, n_off + i * 12) for i in range(normal_bytes // 12)]
        indices = []
        for i in range(tri_count):
            base = t_off + i * 8
            a, b, c = struct.unpack_from("<3H", buf, base)
            indices += [a & INDEX_MASK, b & INDEX_MASK, c & INDEX_MASK]

        meshes.append({"vertices": vertices, "normals": normals, "indices": indices, "flags": flags})
        offset = t_off + tri_count * 8
    return header, meshes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dat")
    parser.add_argument("--key-table-from-lotus", required=True)
    args = parser.parse_args()

    key_table = load_key_table_from_lotus(Path(args.key_table_from_lotus))
    name, payload = extract(Path(args.dat))
    buf, _ = decrypt(payload, key_table)
    header, meshes = read_meshes(buf)

    print(f"zone {name!r}  version={header['version']:#04x}  placements={header['placements']}  meshes={len(meshes)}")
    print(f"  grid {header['grid']}  quadtree@{header['quadtree']:#x}  shortnames@{header['shortnames']:#x}")
    print()

    # The checks that say whether the layout is right rather than merely parsed.
    bad_index = 0
    off_unit = 0
    total_v = total_t = 0
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for mesh in meshes:
        total_v += len(mesh["vertices"])
        total_t += len(mesh["indices"]) // 3
        for idx in mesh["indices"]:
            if idx >= len(mesh["vertices"]):
                bad_index += 1
        for nx, ny, nz in mesh["normals"]:
            if abs(math.sqrt(nx * nx + ny * ny + nz * nz) - 1.0) > 0.02:
                off_unit += 1
        for vx, vy, vz in mesh["vertices"]:
            lo = [min(lo[0], vx), min(lo[1], vy), min(lo[2], vz)]
            hi = [max(hi[0], vx), max(hi[1], vy), max(hi[2], vz)]

    total_n = sum(len(m["normals"]) for m in meshes)
    print(f"  {total_v} vertices, {total_n} normals, {total_t} triangles")
    print(f"  bounds  x {lo[0]:8.1f} .. {hi[0]:8.1f}")
    print(f"          y {lo[1]:8.1f} .. {hi[1]:8.1f}")
    print(f"          z {lo[2]:8.1f} .. {hi[2]:8.1f}")
    print()
    print(f"  indices out of range : {bad_index}   (want 0)")
    print(f"  non-unit normals     : {off_unit} of {total_n}   (want 0)")


if __name__ == "__main__":
    main()
