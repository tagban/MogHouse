# Coordinates and rotation

Every frame of reference this client touches, and how to get between them.
Most of the bugs worth remembering have been one of these applied twice, or
not at all, or applied as a reflection when it should have been a rotation.

## The two frames

**FFXI's frame** is what the DATs store and what the server talks in. Y
increases *downward*: a character standing on Windurst Waters' streets is at
about `y = -5`, and the water under them is at `y = 2.5`.

**The renderer's frame** has Y up, like every other renderer.

    world = (x, -y, -z)

**That is a half turn about X, and it matters that it is.** The obvious
conversion is `(x, -y, z)` - flip the axis that disagrees and leave the rest -
and that is a **reflection**, not a rotation. A reflected world looks almost
right: the terrain is there, the buildings are there, and everything is
mirrored. It was in for a while.

Negating Z as well makes it a rotation, and a rotation is what a change of
handedness needs.

### Reading a position back

The HUD prints `(world.x, -world.z)`, which is `(ffxi.x, ffxi.z)` - the same
pair `!pos` prints server-side. That is deliberate: a coordinate you cannot
compare against the server is a coordinate you cannot check.

## Headings

The server sends a **byte over the full circle**, not degrees or radians. Zero
faces `+x`, and the angle runs the opposite way to the usual convention -
LandSandBoat builds it in `worldAngle` (`src/common/utils.cpp`) as:

    atan2(dz, dx) * -(128 / pi)

Through the half turn about X, that arrives here as:

    heading = pi/2 - direction * 2pi/256

**Not `pi - direction`.** The quarter turn between those two is the difference
between an NPC facing over their counter and facing across it, which is exactly
how the mistake showed up: every shopkeeper in the zone stood at right angles
to the thing they were standing behind. A quarter turn is small enough to look
like a modelling quirk and large enough to be wrong everywhere.

The same correction applies to the player and to every entity. There is one
formula and both sites use it.

Going the other way, to tell the server where we are facing:

    direction = round(((pi/2 - heading) / 2pi) * 256)

## The minimap

The radar samples a top-down image baked once per zone, and places dots by
comparing **world positions**, not screen positions. That ordering is what
keeps the two honest: turning the map happens where radar space becomes world
space, so the map and everything drawn on it turn together by construction
rather than being rotated twice by two pieces of code that might disagree.

Turning it sends screen-up to the direction faced:

    sampled = ( offset.x * cos h + offset.y * sin h,
               -offset.x * sin h + offset.y * cos h )

The compass letters use the inverse of that, because they answer the opposite
question - not "what world point is at this screen position" but "where on
screen does this world bearing sit".

### The bake's left-right swap

`mh::orthographic(half, -half, ...)` swaps left and right on purpose. Looking
straight down with up = `+z` makes `cross(forward, up) = -x`, so a plain
projection puts `+x` on the **left** of the image.

**This is correct, and it has been measured against something outside the
renderer.** Take the zone's own MZB placements, put them through `mapUv`'s own
formula, and ask how much of that geometry has terrain drawn on it in the baked
image:

| bake | covered |
|---|---|
| with the swap | **99.1%** |
| without it | 65.6% |

The MZB is the server's and Square's; nothing in this client produced it. That
is the difference between this check and the one it replaces, which compared
the bake against `rasteriseWalkable` - a mask referenced from no live code path
at all. Two mirrored things agree with each other perfectly.

The swap was removed once on the theory that the old check had been
self-consistent and therefore worthless. The theory about the check was right
and the conclusion was wrong: the swap was doing real work, and the measurement
above is how that was established rather than argued.

### Reproducing that check

    MOGHOUSE_MAP=map.bmp build-renderer/moghouse-renderer.exe <zone DAT> --frames 1

then compare the bitmap against the placements in the decrypted MZB. Anything
that disagrees with the world by a mirror scores in the sixties; anything that
agrees scores in the high nineties. There is no middle ground, which makes it a
good test.

## What has been wrong before

Kept because each looked plausible and cost real time.

- **`(x, -y, z)`** for the world. A reflection. Everything is present and
  everything is mirrored, which reads as "the art is slightly off" rather than
  as a broken transform.
- **`pi - r`** for headings instead of `pi/2 - r`. A quarter turn. NPCs face
  across their counters.
- **Self-consistency as proof.** Two halves of one client agreeing tells you
  they were written by the same person, not that either is right.
