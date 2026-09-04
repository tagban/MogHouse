# Skeletons: finding a bone by shape

A skeleton is an `0x29` chunk: a flat list of bones, each carrying a parent
index, a quaternion and a translation. That is all. **Bones have no names**,
so anything that wants a particular one - a head to turn, a hand to hang a
weapon from - has to find it by shape.

## The head is the bone with eyes

The head's index is different for every race, so it cannot be a constant:

| race | bones | head |
|---|---|---|
| hume male | 94 | 52 |
| hume female | 97 | 30 |
| elvaan male | 99 | 30 |
| elvaan female | 99 | 57 |
| tarutaru | 93 | 7 |
| mithra | 108 | 40 |
| galka | 107 | 40 |

What *is* the same everywhere is that a head has eyes, and eyes are a mirrored
pair of children - same height, same depth, equal and opposite z. So:

> the head is the bone with a mirrored pair of children, highest up the body.

That finds all seven, and `mh::headBone` in `renderer/character.cpp` is exactly
that rule. Most heads have several such pairs, being eyes and ears together;
a tarutaru has four, which is why their ears are the size they are.

The "highest up the body" half matters because hips and ribcages also have
mirrored children. **Y runs negative upward** in this space, so highest means
smallest - a hume male's feet sit near 0.0 and their head at -1.63.

## Which way a head nods

The same pair settles it. The eyes are at plus and minus z, so **z is the
ear-to-ear axis**, and a nod is a rotation about it. No separate discovery is
needed: the thing that identifies the head also orients it.

Which of the two directions is *up* is the one part the bind pose does not
answer, because it depends on which way +x runs and the numbers there are small
and inconsistent. It was settled by looking: the first attempt had every head
tilting politely away from whoever was talking to them, so a positive turn
about z tips the face **back**, and looking up is the negative one.

`mh::pitchHead` rotates the head and every bone descending from it about that
axis, through the head's own position. The whole subtree has to move or hair
and helmets stay behind in mid-air. Bones are stored parents-first, so one
downward pass reaches every descendant.

## Reading it back

```
ffxi-chardump <race base>.DAT           # prints the head bone it found
MOGHOUSE_BONES=1 ffxi-chardump <dat>    # every bone: index, parent, bind position
```

The race base files are in [characters.md](../characters.md) - 7072 for a hume
male, 26352 for a galka.

## Where this is used

Turning to face somebody who talks to you. The body turn is a yaw on the whole
model and needs no skeleton at all; the head tilt is this. Both are the
client's own work - the server never says to turn, and retail's client does it
itself.

Worth knowing that only entities skinned per frame can have their head moved.
Characters sharing a look share one skinned mesh drawn as instances, so a bone
moved for one would move for all of them; the per-entity animation path is what
makes this safe.
