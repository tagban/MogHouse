# Standing on the world

The collision meshes have been in hand since the zone reader landed — they are
what the untextured collision view draws, and what made it obvious the
walkable surface was in there. Nothing ever queried them. This turns them into
two questions:

    groundAt(x, z, near)    how high is the floor here
    move(from, to, radius)  where does this step actually end

Read by `renderer/collision.cpp`, probed by `ffxi-collisiondump`.

## Why the DATs rather than the server's navmeshes

LandSandBoat ships Recast navmeshes per zone, and PortJeuno's C# client already
reads them for server-authoritative movement. The renderer deliberately does
not.

- The navmeshes are a **derived** artifact, generated from these same DATs.
  Using them means inheriting whoever ran that generation's parameters.
- A standalone client should not need a server's files to know where the floor
  is.
- When Square Enix patches a zone the DAT changes and the collision changes
  with it. A navmesh stays stale until someone regenerates it.
- Navmeshes exist only for zones the server bothered to generate.
- It is what the retail client itself does.

What the navmeshes are genuinely better at, and where they stay: Recast has
already solved *walkability* rather than just geometry — agent radius, step
height, slope limits, and pruning ledges too small to stand on are baked in.
And Detour gives A\* pathfinding, which raw triangles do not. If click-to-move
or NPC routing ever lands, that is the tool for it.

The cost of choosing the DATs is that slope, step height and radius are ours to
get right rather than Recast's. They are the three constants at the top of
`collision.cpp`.

## How it works

Every collision mesh instance is baked into world space once — the same
transform and Y flip `buildZoneMesh` uses, so the query and the drawing cannot
disagree — and bucketed into a grid on x and z. East Sarutabaruta is 427,794
triangles across 1,700 units; a linear scan per step would be hundreds of
thousands of tests a frame, and the grid makes it tens.

A triangle is **walkable** if `|normal.y| >= 0.64`, about 50°. The winding is
not consistent across a zone, so a face pointing down is as much a floor as one
pointing up — hence the absolute value.

`groundAt` takes the highest walkable surface at or below the query point, so
standing on a bridge does not drop you to the riverbed. It allows a small
tolerance *above* the query point, because walking up a slope puts the next
step slightly higher than the current one.

## Three things that were wrong first

**A destination test is not collision.** Asking "is the end point walkable"
passes straight through a wall, because the floor on the far side is walkable.
That is exactly how the C# client walked a character through a wall. `move`
tests the path.

**The step height and the ground tolerance have to be the same number.**
Blocking on anything taller than 0.3 units meant the ankle-high rocks Sarutabaruta
is covered in counted as walls, and a character stopped dead three units into
open country. Anything the ground query would happily carry you onto must not
block you.

**A hit rate does not tell you the wall test is sane.** 36% of probes over the
bounding box find ground, which sounds low until you notice the box is a
rectangle and the zone is not. The measure that actually catches an over-eager
wall test is whether you can *go* anywhere:

| zone | triangles | walls | ground found | can walk 10 units in 3+ of 4 directions |
| --- | --- | --- | --- | --- |
| East Sarutabaruta | 427,794 | 227,791 | 36% | **87%** |
| Bastok Markets | 66,537 | 32,387 | 35% | **89%** |

## Dropping a character

A zone's bounding box is a rectangle and the zone is not, so an obvious
starting point like the centre often has nothing under it. `nearestGround`
searches outward a grid cell at a time rather than leaving the character in the
air.

With no collision at all — a zone we could not parse — movement is left
unconstrained rather than trapping the character somewhere arbitrary.

## Not done yet

- Zone lines as spawn points. LandSandBoat's `data/zones/<zone>/zone.yaml`
  carries `zonelines:` with real in-zone coordinates, and `tools/zonenames.py`
  already maps our chunk ids to its zone folders.
- Walking from one zone into the next, which is the same data.
- Falling. A character walked off a ledge stops at the edge instead of
  dropping.
- Ceilings. Only the floor and the walls are consulted.
