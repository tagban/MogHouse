"""Write a .water file for every zone the server has a collision mesh for.

Water is a material on each collision triangle - ShallowWater and DeepWater in
LandSandBoat's own TerrainType - so the surfaces are lifted out of the server's
`.ximesh` files and written world-space ahead of time. See tools/ximesh.py for
the reading and the waterline, and docs/water-candidates.md for why the two
earlier approaches (a model name, the MZB's per-cell height) were both wrong.

    python tools/makewater.py                       # into renderer/assets/water
    python tools/makewater.py --out some/dir

Roughly 50MB across 185 zones, which is why the output is not in git: it is
derived from data the server already ships, and regenerating it takes about a
minute. Run this once after cloning, and again if the server's meshes change.
"""

import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ximesh


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("renderer", "assets", "water"))
    ap.add_argument("--root", default=os.environ.get("MOGHOUSE_FFXI_XIMESHES", ximesh.DEFAULT_ROOT))
    args = ap.parse_args()

    meshes = sorted(glob.glob(os.path.join(args.root, "*.ximesh")))
    if not meshes:
        raise SystemExit(f"no .ximesh files under {args.root} - set MOGHOUSE_FFXI_XIMESHES")

    os.makedirs(args.out, exist_ok=True)
    wet = dry = failed = 0
    triangles = 0
    for path in meshes:
        zone = os.path.splitext(os.path.basename(path))[0]
        out = os.path.join(args.out, zone + ".water")
        try:
            count = ximesh.emit(zone, ximesh.load(zone, args.root), out)
        except Exception as problem:            # one bad zone is not worth stopping for
            failed += 1
            print(f"  {zone}: {problem}")
            continue
        if count:
            wet += 1
            triangles += count
        else:
            # A dry zone needs no file, and an empty one would only be opened.
            os.remove(out)
            dry += 1

    print(f"{wet} zones with water ({triangles} triangles), {dry} dry, {failed} failed -> {args.out}")


if __name__ == "__main__":
    main()
