#!/usr/bin/env python3
"""What the zone file puts near a point, for comparing against a retail client.

    python tools/nearby.py 238 -57.3 10.2 -93.2            # world coords, as the viewer logs them
    python tools/nearby.py 238 --lsb -57.3 -10.2 93.2      # coords as LandSandBoat stores them
    python tools/nearby.py 238 -57.3 10.2 -93.2 --radius 40

World is (x, -y, -z) relative to the DAT, so --lsb flips y and z back.

Needs a decrypted MZB, which ffxi-datdump writes:

    MOGHOUSE_MZB_DUMP=zone238.mzb MOGHOUSE_FFXI_KEYTABLE=keys/mzb_key_table.bin \
        build-renderer/ffxi-datdump.exe ".../ROM/0/78.DAT"
"""
import argparse
import collections
import math
import os
import struct
import sys

PLACEMENTS_AT = 32
STRIDE = 0x64


def placements(path):
    buf = open(path, "rb").read()
    count = struct.unpack_from("<I", buf, 4)[0] & 0x00FFFFFF
    # The word at 20 is where the table ends, which is what fixes the stride.
    ends = struct.unpack_from("<I", buf, 20)[0]
    stride = STRIDE if PLACEMENTS_AT + count * STRIDE == ends else (ends - PLACEMENTS_AT) // count
    for i in range(count):
        o = PLACEMENTS_AT + i * stride
        name = buf[o:o + 16].split(b"\x00")[0].decode("ascii", "replace").strip()
        x, y, z = struct.unpack_from("<3f", buf, o + 16)
        far = struct.unpack_from("<f", buf, o + 64)[0]
        yield name, x, y, z, far


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zone", type=int)
    ap.add_argument("x", type=float)
    ap.add_argument("y", type=float)
    ap.add_argument("z", type=float)
    ap.add_argument("--lsb", action="store_true", help="coordinates are LandSandBoat's, not the viewer's")
    ap.add_argument("--radius", type=float, default=30.0)
    ap.add_argument("--mzb", help="decrypted MZB (default: zone<id>.mzb beside this script)")
    args = ap.parse_args()

    here = (args.x, args.y, args.z) if args.lsb else (args.x, -args.y, -args.z)
    path = args.mzb or "zone%d.mzb" % args.zone
    if not os.path.exists(path):
        sys.exit("no decrypted MZB at %s - see the header of this file for how to write one" % path)

    near = sorted((math.dist((x, y, z), here), n, x, y, z, far)
                  for n, x, y, z, far in placements(path)
                  if math.dist((x, y, z), here) <= args.radius)

    print("%d placements within %.0f units of (%.1f, %.1f, %.1f) in DAT space"
          % (len(near), args.radius, *here))
    for d, name, x, y, z, far in near:
        note = "   past its own %.0f cutoff" % far if d > far else ""
        print("  %6.1f  %-16s %8.2f %7.2f %8.2f%s" % (d, name, x, y, z, note))

    if near:
        print("\nby model:")
        for name, n in collections.Counter(r[1] for r in near).most_common():
            print("    %-16s x%d" % (name, n))


if __name__ == "__main__":
    main()
