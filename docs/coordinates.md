# Which way is up

FFXI's vertical axis points **down**. The renderer wants Y up. The conversion,
applied everywhere FFXI coordinates cross into MogHouse:

    world = (x, -y, -z)

That is a half turn about X. Determinant +1, so it is a rotation and nothing
changes handedness.

The obvious conversion — negate the vertical and leave the rest alone — is
`(x, -y, z)`, and it is wrong. Determinant −1: a reflection. It leaves a world
that is mirrored east to west, and MogHouse rendered that way for weeks.

## Why it took so long to see

A mirrored city is still a city. Every building is still a building, the
streets still connect, the water still sits in the basin that holds it.
Nothing looks broken. It was caught only when the user put a screenshot of the
retail client beside ours and noticed that the water beside the Bastok Markets
auction house had swapped sides.

Worse, it survived a deliberate correctness pass. The check was: bake the map,
build a walkable mask from the collision data, and score how much of one lands
on the other. It scored 100%, then 99.8%. Both artifacts were built from the
same world through the same `toWorld`, so both were mirrored **the same way**,
and they agreed with each other perfectly while both being wrong.

That is the general trap: two of our own outputs agreeing tells you they were
produced consistently. It says nothing about whether either matches reality.

An attempted fix negated `m[0]` in `perspective` and `orthographic`, which
un-mirrored the picture and left the world coordinates mirrored underneath it.
Everything that reaches the screen through a projection looked right;
everything that does not — where a position from the server lands, where the
water is relative to a building — was still wrong, and now inconsistent with
the view. It was reverted.

## Checking it against something we did not produce

`tools/npcground.py` uses LandSandBoat's NPC placements. They are real FFXI
coordinates authored against the real geometry, by someone other than us, and
they carry an assertion we can test: an NPC stands on the floor.

So probe our collision at each NPC's position under both candidate transforms,
and ask which one puts them on a floor near the height they claim.

    python tools/npcground.py Bastok_Markets 235

                          rotation   reflection
    Bastok Markets          85.0%       11.3%
    East Sarutabaruta       96.1%        9.1%
    Windurst Waters         86.8%       16.5%
    Southern San d'Oria     91.2%       17.4%

1,859 placements. Under the reflection, most of them stand over no floor at
all. The residual under the rotation is NPCs on furniture, on platforms, and a
few the server places slightly off the ground on purpose.

Run this after touching the conversion. It is the only check here that can
fail for the right reason.

## Where the conversion lives

| Place | What crosses |
| --- | --- |
| `renderer/zonemesh.cpp` `toWorld` | zone geometry and normals |
| `renderer/collision.cpp` `toWorld` | collision triangles |
| `renderer/character.cpp` `toVertex` | skinned character vertices and normals |
| `Program.cs` `LiveRadar.Open` | the spawn position from the server |
| `Program.cs` `LiveRadar.Publish` | radar dot positions |
| `Program.cs` `LiveRadar.Position` | the position reported back to the server |

The projections in `renderer/linalg.h` are plain and flip nothing. If a fix for
an orientation problem wants to go in there, it is compensating for something
wrong further up.
