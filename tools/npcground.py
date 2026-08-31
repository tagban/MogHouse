"""Checks our zone geometry against coordinates we did not produce.

Every consistency check so far compared our own outputs to each other - the
baked map against a mask built from the same collision data. Those agree with
each other whatever frame they are both wrong in, which is how a globally
mirrored world passed at 100% for weeks.

The server's NPC placements are an outside reference. They are real FFXI
coordinates, authored against the real geometry, and they say where the ground
is: an NPC stands on it. So probe our collision under each candidate transform
and see which one puts the NPCs on the floor.

    python tools/npcground.py Bastok_Markets 235

The two candidates differ only in the sign of the depth axis:

    rotation    world = (x, -y, -z)   a half turn about X, determinant +1
    reflection  world = (x, -y,  z)   a mirror, determinant -1

Both leave a plausible-looking city standing. Only one of them is the one
Square Enix built.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

MOGHOUSE = Path(__file__).resolve().parent.parent
PROBE = MOGHOUSE / "build-renderer" / "ffxi-collisiondump.exe"
SERVER = Path(r"C:\Users\Gaming\Desktop\LandSandBoat")
INSTALL = Path(r"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI")

# An NPC standing on the floor is within about a metre of it. Wider than that
# and a wall top or a rooftop starts counting as a hit.
TOLERANCE = 1.5


def npc_positions(zone_name):
    """Every `at: [x, y, z, rot]` in the zone's NPC data."""
    found = []
    for path in sorted((SERVER / "scripts" / "zones" / zone_name).rglob("*.yaml")) + \
                sorted((SERVER / "data" / "zones" / zone_name).rglob("*.yaml") if
                       (SERVER / "data" / "zones" / zone_name).exists() else []):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(
                r"at:\s*\[\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)", text):
            found.append(tuple(float(g) for g in match.groups()))
    return found


def probe(zone_dat, points):
    """Ground height nearest each (x, z, y), or None where there is no floor."""
    stdin = "".join(f"{x} {z} {y}\n" for x, z, y in points)
    result = subprocess.run([str(PROBE), str(zone_dat), "--probe"],
                            input=stdin, capture_output=True, text=True)
    heights = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        # A probe line is "x z ground" - anything whose first two fields are
        # not numbers is the tool's own header, not an answer.
        try:
            float(parts[0])
            float(parts[1])
        except ValueError:
            continue
        heights.append(None if parts[2] == "none" else float(parts[2]))
    return heights


def main():
    zone_name = sys.argv[1] if len(sys.argv) > 1 else "Bastok_Markets"
    zone_id = int(sys.argv[2]) if len(sys.argv) > 2 else 235

    npcs = npc_positions(zone_name)
    if not npcs:
        print(f"no NPC coordinates found for {zone_name}")
        return 2
    print(f"{zone_name}: {len(npcs)} NPC positions from the server")

    zone_dat = zone_path(zone_id)
    if zone_dat is None:
        print(f"zone {zone_id} is not installed")
        return 2

    # All four horizontal sign choices. Two of them are rotations - a half
    # turn about X and a half turn about Z - and both have determinant +1, so
    # "is it a rotation" does not pick between them. The other two are mirrors.
    # Sign choices AND axis swaps. Four sign combinations times swapped or
    # not is eight, of which four are rotations. Testing only the signs, as
    # this did at first, cannot see a zone turned a quarter turn - the very
    # thing you get by reading a horizontal pair in the wrong order.
    candidates = {}
    for swap in (False, True):
        for sx in (1, -1):
            for sz in (1, -1):
                name = ("swap " if swap else "     ") + f"x*{sx:+d} z*{sz:+d}"
                if swap:
                    candidates[name] = [(sx * z, sz * x, -y) for x, y, z in npcs]
                else:
                    candidates[name] = [(sx * x, sz * z, -y) for x, y, z in npcs]

    for label, points in candidates.items():
        heights = probe(zone_dat, points)
        if len(heights) != len(npcs):
            print(f"{label}: probe returned {len(heights)} of {len(npcs)}")
            continue
        on_floor = 0
        missing = 0
        error_total = 0.0
        for (_, y, _), ground in zip(npcs, heights):
            if ground is None:
                missing += 1
                continue
            error = abs(ground - (-y))
            error_total += min(error, 50.0)
            if error <= TOLERANCE:
                on_floor += 1
        share = 100.0 * on_floor / len(npcs)
        print(f"  {label}: {on_floor}/{len(npcs)} on the floor ({share:.1f}%), "
              f"{missing} over no floor at all, mean miss {error_total / len(npcs):.1f}")


def zone_path(zone_id):
    """The zone DAT, through the same file table the client uses."""
    sys.path.insert(0, str(MOGHOUSE / 'tools'))
    from filetable import FileTable
    found = FileTable(INSTALL).path(zone_id + 100)
    return found if found and Path(found).exists() else None


if __name__ == "__main__":
    os.environ.setdefault("MOGHOUSE_FFXI_KEYTABLE", str(MOGHOUSE / "keys" / "mzb_key_table.bin"))
    sys.exit(main() or 0)
