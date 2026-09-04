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
  sounds like one, and it is not in these headers.
