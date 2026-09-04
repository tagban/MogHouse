# Audio: `.bgw` music and `.spw` sound effects

Two containers, one codec, and a header that is the same twice with everything
moved four bytes.

Read from a retail install on 2026-09-03. The music half was already decoded -
`renderer/ffxi/bgw.cpp` - and this is the sound effects beside it, checked
across 1,471 of the 11,862 `.spw` files an install carries.

## Where they are

```
sound/win/music/data/musicNNN.bgw      the music, ~111 tracks
sound/win/se/seNNN/seNNNNNN.spw        the sound effects, 11,862 of them
```

Expansions continue the numbering into `sound2` through `sound9`, which is why
finding a track is a search rather than a computation - see
`FfxiMusicFile.Resolve`.

## The header

Forty-eight bytes. The two formats agree on every field and disagree on where
it is: `.spw` sits four bytes earlier throughout, because its magic is eight
bytes where the music's is twelve.

| field | `.bgw` | `.spw` |
|---|---|---|
| magic | `BGMStream` at +0x00 | `SeWave` at +0x00 |
| payload bytes | — | +0x08 |
| track number | +0x14 | — |
| count | +0x18 | +0x14 |
| loop point | +0x1C | +0x18 |
| sample rate, first half | +0x20 | +0x1C |
| sample rate, second half | +0x24 | +0x20 |
| data offset | +0x28 | +0x24 |
| channels | — | +0x2A |

A loop point of `0xFFFFFFFF` means the sound does not loop. Twenty-two of six
hundred effects sampled do loop - a torch, a fountain, a waterfall.

### The sample rate is split in two

Stored as a pair that sums to it, wrapping. Nobody knows why; it is not a
checksum, because either half is free. Every file in a retail install adds up
to a rate, and both formats do it.

The music is 44,100 with 29 tracks at 48,000 - playing those at a fixed rate
runs them nine per cent slow, which is flat rather than obviously broken and
is why it went unnoticed for a while. **Every sound effect sampled is 48,000.**

### The count means different things

For `.bgw` it is always ADPCM blocks. For `.spw` it depends on what follows,
and the arithmetic is what says which:

| payload ÷ count | what it is |
|---|---|
| 9 | ADPCM, mono - a 9-byte block is 16 samples |
| 18 | ADPCM, stereo |
| 2 | PCM16, mono - count is frames, not blocks |
| 4 | PCM16, stereo |

Of 1,471 files sampled: 1,110 mono ADPCM, 119 stereo ADPCM, 85 mono PCM16, 12
stereo PCM16, and 145 that fit none of those and are not yet understood.

**Detect the format by that division rather than by a flag.** The byte at
+0x29 looked like a codec field and is not - it takes the value 0 for both
ADPCM and PCM16 files, and 128 or 32 for others that are ordinary ADPCM. The
division is self-checking: it either divides exactly or the file is not what
you thought.

## The codec

Sony ADPCM, the four-coefficient scheme the PlayStation used, identical in
both formats. A block is nine bytes a channel: one byte of predictor and shift
- predictor in the high nibble, shift in the low - then eight bytes holding
sixteen four-bit samples.

Stereo interleaves by block rather than by sample: nine bytes of left then
nine of right, not alternating nibbles.

Decoding cannot start anywhere but the beginning, because each sample is a
difference from the two before it. That is why looping seeks to a block
boundary and keeps the running history rather than resetting it.

## What is not known

- The 145 files in the sample whose payload divides by neither 9, 18, 2 nor 4.
  They read as having a count far larger than the data supports.
- What +0x29 actually is, and the other bytes around +0x28.
- **Which sound belongs to which event.** The files are numbered and nothing
  read so far says that `seNNNNNN` is the noise a worm makes coming out of the
  ground. That mapping is the thing standing between a decoder and a game that
  sounds like one.

  Ruled out on 2026-09-03, so nobody pays for it twice:

  * **Not in these headers.** Every field is accounted for above except a few
    bytes around +0x28, which are too few to name an event.
  * **Not in the animations.** An MO2 chunk leaves 6 to 24 bytes unread past
    the bone tracks, which looks promising and is not - the content is float
    bit patterns continuing the channel pool and the size tracks the frame
    count, so it is the pool's tail and padding rather than events.
  * **Not in `sound/win/se/_bitInfo.inf`.** Four bytes, `03 00 00 00`, a
    version.

  **Solved for the sounds a creature owns: chunk type 0x3D.** A DAT declares
  the sounds it uses, and the chunk's own four-character name says what each
  one is for.

  | offset | size | field |
  |---|---|---|
  | 0x00 | 8 | `SeSep  ` |
  | 0x08 | 4 | sound id, u32 |

  Past the usual 16-byte chunk header. The id becomes a path by
  `se{id/1000:03d}/se{id:06d}.spw` - 252003 is `se252/se252003.spw`. All 21 in
  the worm at model 426 resolve to files that exist.

  The names are a fixed vocabulary, counted across 614 creature models:

  ```
  idl1 idl2      idle          atk1-atk4  attacking
  dam1-dam4      taking a hit  swy1-swy3  swaying
  ded1-ded3      dying         sdam skaz shit  shared, in se006
  ```

  **No creature declares a spawn sound.** Not one of the 614 has a `pop` or
  anything like it, which is why a worm's own DAT gives only combat noises.
  Whatever plays when one comes out of the ground is not the creature's - the
  next place to look is the zone, where a DAT carries hundreds of these
  against a creature's twenty: West Ronfaure has 805.

  A creature's own folder is not its family number. Model 426 is a worm and
  its sounds are in `se252`, while the worm family is 258 - and `se258` exists
  with sixteen files of its own.

  **The other lead is chunk type 0x07, the Scheduler.** A creature's model DAT
  holds dozens - the worm at model 426 has 38 - and they are named after
  events rather than after anything graphical:

  ```
  pop0  dead  corp  damg  atk0  pary  gurd  shot  cast  init  sway ...
  ```

  `pop0` is a spawn: "pop" is the word FFXI uses for a mob appearing, and
  every creature looked at has one. They are opcode streams with the same
  shape as the generators MogHouse already reads - four stream offsets, then
  `op / length-in-dwords / pad / payload` - and nothing reads them yet.

  `pop0` on the worm decodes to three opcodes, `0x01`, `0x29` and `0x28`, and
  none of their payloads is obviously a sound number. So the trail stops
  there, but it stops somewhere specific.

  The test worth running next: decode one scheduler across many creatures and
  look for an opcode whose payload changes per creature and lands in 0..11862.
  A sound id must vary by creature and fit the library; a colour or a scale
  will do neither. `dead` and `damg` are better subjects than `pop0` because
  every creature certainly makes a noise for those.

  An earlier note here said the mapping was most likely compiled into the
  client and could not be derived. That was wrong: it is in the DATs, as 0x3D
  above. Recorded because it was wrong for a specific reason worth avoiding -
  0x3D was sitting in a chunk histogram already printed, unrecognised, and the
  conclusion was reached by running out of ideas rather than by looking.

  `ffxi-sounddump --wav` converts a folder so it can be judged by ear, which
  is still how the last step gets settled. About 172 of the 393 folders hold
  exactly sixteen files.
