# MO2 — animation

Chunk type `0x2B`. One clip: an idle, a walk, a sword swing. Read by
`renderer/ffxi/mo2.cpp`.

## Header, 10 bytes, packed to two

| offset | size | meaning |
| --- | --- | --- |
| 0 | 2 | unidentified, zero in everything measured |
| 2 | 2 | element count |
| 4 | 2 | frame count |
| 6 | 4 | speed, a multiplier on thirty frames a second |

## Element, 84 bytes

One per animated bone.

| offset | size | meaning |
| --- | --- | --- |
| 0 | 4 | bone index |
| 4 | 16 | rotation channel indices, x y z w |
| 20 | 16 | rotation constants |
| 36 | 12 | translation channel indices |
| 48 | 12 | translation constants |
| 60 | 12 | scale channel indices |
| 72 | 12 | scale constants |

Each of the ten channels is either a constant — index ≤ 0, use the constant —
or an index into a pool of floats, where frame *f* reads `pool[index + f]`.

**The pool is indexed in floats from the start of the element block**, so the
elements and the pool share an origin. In the hume male's `idl0` the first
channel index is 336, which is byte 1354, which is exactly where the sixteen
elements end.

## Two things the format does not say

**A negative rotation index marks a bone the animation does not touch.** It
still occupies an element, carrying `INT32_MIN` in all four rotation channels.
Reading those as data produces a plausible-looking limb in the wrong place.

**Frame zero is not part of the animation.** Every clip measured starts at
frame one, so a 16-frame chunk is a 15-frame animation.

## Composing

The animation turns the bone from where it rests rather than replacing it:

    local rotation    = animated rotation × rest rotation
    local translation = rest translation + animated translation
    local scale       = animated scale

then accumulated down the parent chain as usual. Replacing would collapse the
skeleton — most animated bones carry no translation of their own, so they would
lose the offsets that hold the body together, the hip offset among them.

An idle touches sixteen of the hume male's ninety-four bones. Every other bone
keeps its rest pose.

## Playback

Speed is a multiplier on thirty frames a second, so `idl0` at 0.25 runs at 7.5
and `wlk0` at 0.5 runs at 15. That is slow enough that stepping between frames
is visible, so frames are interpolated — slerp for the rotation, linear for
translation and scale.

## Verified

The hume male's own file yields 130 animations, including `idl0`, `std0`,
`wlk0`, `run0` and `jmp0`. `wlk0` is 18 frames at 15 a second and renders as a
walk cycle: stride, foot plant, torso lean.
