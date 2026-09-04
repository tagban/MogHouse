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

## How a sound is chosen

There is no table mapping creatures or places to sounds, and none is needed:
**a DAT declares the sounds it uses**, and the 0x3D chunk's own four-character
name says what each one is for. The client asks the model. So does this -
`renderer/ffxi/soundrefs.cpp`.

### A creature says what it can do

Counted across 614 models, every creature carries the same vocabulary:

```
idl1 idl2   standing about      atk1-atk4  attacking
dam1-dam4   taking a hit        swy1-swy3  swaying
ded1-ded3   dying               sdam skaz shit   shared weapon sounds, se006
```

Anything outside it is worth looking at, and that is how the thing this whole
search was for turned up. The worm at model 426 has exactly two extras, 17024
and 17025, and they are it coming out of the ground and going back under -
confirmed by ear against the retail client.

Which makes the list of creatures that burrow a query rather than a guess:
the ones that reference those sounds. Models 424 through 427 reference both;
2413 references only the going-under. An earlier list built from the server's
mob names was wrong on three of its four entries.

### A place says what can be heard there

Positional ambience is a sound chunk sharing a directory with generators - and
a generator carries a position, which is how the same directories already give
this renderer its torches. West Ronfaure:

| directory | sounds | generators |
|---|---|---|
| `mode/ligh/taki` | 2024 | 56 |
| `effe/aose` | 2079-2086 and more | 68 |
| `effe/aotr` | 2124, 2126, 2146, 2159 | 136 |
| `weat/{fine,clod,mist,suny}` | 1005, 1007 | 3 each |

`taki` is Japanese for waterfall: one sound, fifty-six places to hear it from.

### A chunk outside the vocabulary is named after its sound

`idl1` and `atk1` are mnemonics, but the worm's two extras are named `7024` and
`7025` - the tail of the sound numbers themselves, 17024 and 17025. So a chunk
name that looks like digits is the signal that a sound is specific to this one
model rather than part of what every creature has, and there is no mnemonic to
look up because there is nothing general to name.

### Two flags tell you which kind a sound is

Both hold across every sound checked, and they agree with each other:

| | loops | channels |
|---|---|---|
| zone-wide ambience (1005, 1007) | yes | **stereo** |
| positional ambience (2024, a waterfall) | yes | mono |
| an event (17024, a worm surfacing) | no | mono |

Stereo cannot be panned to a cliff edge and mono can, so the channel count is
not decoration - it says whether a sound has a place. Length agrees too: the
ambience runs 9 to 18 seconds and the events run about two.

### Reading it back

`ffxi-sounddump --refs <file.DAT>` prints what a model or a zone declares,
marking anything outside the standard creature set:

```
  ded3  se/se252/se252016.spw     mimi
  7024  se/se017/se017024.spw     mimi   <- not the standard set
  7025  se/se017/se017025.spw     mimi   <- not the standard set
```

## How the client uses this

`renderer/viewer.cpp` collects a `SoundEmitter` wherever a 0x3D sound and a
generator share a directory, then each frame holds one voice per distinct
sound at the distance of its *nearest* placement - not one per emitter, since
fifty-six copies of one waterfall playing over each other would be both wrong
and loud. `mh::Sounds::hold` keeps them going by topping the stream up from
the loop point before it runs dry.

Both rules above are enforced by asking the file rather than by a list:
`hold` refuses anything that does not loop, and a stereo sound is played at
full volume everywhere instead of falling off. Loading West Ronfaure:

```
ambience: 2230 emitters, 15 distinct sounds
ambience: se001005 held, zone-wide (stereo)
ambience: se001007 held, zone-wide (stereo)
ambience: se002024 held, positional (mono)      <- the waterfall
ambience: se002079 does not loop, not ambience
...
```

Fifteen candidates, and the files themselves picked the three that are
ambience. `MOGHOUSE_AMBIENCE_REACH` sets how far a positional one carries, in
yalms - the game's coordinates are yalms 1:1, with no conversion anywhere
between the DAT, the server and the renderer. LandSandBoat divides raw x/y/z
by a yalms-per-cell constant and compares squared raw distances against
squared yalm values directly.

It defaults to 30, which is a guess: it is roughly the radius at which the
server bothers sending action packets. The falloff is squared, so the audible
part is much tighter than the number suggests - 0.44 at ten yalms, 0.25 at
fifteen, and effectively silent past twenty.

**One radius for every emitter is the weak part.** A campfire and a waterfall
are given the same reach. A per-emitter radius may well be in the part of the
0x3D chunk body that is still unread - see below.

A zone keeps ambience under all four of its weathers and only one is up, so
the other three are skipped exactly as their skies are. It changes nothing in
West Ronfaure, where all four name the same wind, but a zone whose rain sounds
different from its sunshine would otherwise play both at once.

## What the folders hold

Built from watching the retail client and from what the DATs reference. A
folder is not a mob family - `se258` is bats though 258 is the worm family -
so this is observation rather than a rule.

| folder | holds |
|---|---|
| `se000` | interface and the common player sounds - menus, walking |
| `se006` | weapons: strikes, drawing and sheathing, a monster falling |
| `se001` | a zone's ambience - wind and the like, on a clock |
| `se002` | more zone sound, referenced by id rather than by a name |
| `se017` | unknown; the worm at model 426 references two of them |
| `se100`-`se140` | footsteps by terrain, which is what a zone's hundreds of references are |
| `se252` | one worm's own set - idle, attack, damage, sway, death |
| `se258` | bats, despite 258 being the worm family |

### A zone's ambience is on a clock

The zone references into `se001` are named for the hour they belong to. West
Ronfaure's three read `0600`, `1800` and `0000` - dawn, dusk and midnight -
and what plays is wind, confirmed by ear against the retail client.

That is the whole scheme for ambience: the name is when, the id is what. It
also means ambient sound is buildable now rather than blocked, since the
pieces are already here - the loop point in a `.spw` header, and a mixer that
holds twenty-four voices.

Southern San d'Oria's are named for their own ids instead, which is a
different arrangement in the same folder and is not understood.

## Sounds identified by watching the retail client

The reliable way to learn what a sound is: run Process Monitor against the
retail client with a filter of `Path contains .spw`, clear the log, do the
thing, and read the path. The client opens these when it plays them rather
than caching a zone's worth at load, so the file that appears is the file you
just heard.

| id | what it is | how it is referenced |
|---|---|---|
| 000003 | walking | zone footstep tables |
| 000011 | menu select | — |
| 006015 | a monster falling | shared, `se006` |
| 006042 | sword strike | shared, `se006` |
| 006043 | sword put away | shared, `se006` |
| 006065 | sword drawn | `sinr`, on 102 creature models |

006065 settles the `s` prefix: `sinr` is unsheathing, not "in" as in
appearing, which is what it was read as here first.

It also leaves a loose end. `sinr` and `sotr` point at 006065 and 006066, an
adjacent pair, and 006065 is confirmed as drawing a sword - but the sound
confirmed as putting one away is 006043, not 006066. Two pairs, then, and
what separates them is unknown: most likely one belongs to the weapon a
creature carries and the other to the player's, or they differ by weapon
type. Watching a few weapons drawn and sheathed in turn would settle it.

### An indoor variant

`weat/<weather>/indo` holds its own sound - 1056 in West Ronfaure, against
1005 and 1007 outside. Presumably what the weather sounds like heard from
inside. Not used yet: it needs to be known whether the camera is under a roof,
and that is not tracked.

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

  The `s` names are a shared set in `se006` and the prefix is the giveaway -
  they are weapon and movement sounds, not creature ones. `sdam`, `shit` and
  `skaz` are sword damage and hits; `sinr` and `sotr`, which pair to
  consecutive ids on 102 models and look like "in" and "out", are sheathe and
  unsheathe. Confirmed by ear: they are swords and footsteps.

  **No creature declares a spawn sound.** Not one of 614 has a `pop` or
  anything like it, and a sweep of every non-standard name across 3,200 model
  slots turns up nothing that could be one. So whatever plays when a worm
  comes out of the ground is not the creature's.

  Where it is not, each checked rather than assumed:

  | | |
  |---|---|
  | the creature's own DAT | only idle, attack, damage, sway, death |
  | zone DATs | 805 in West Ronfaure, all footsteps by terrain - folders 100-140 |
  | the shared effects file | 98 refs, generic, numeric names in folders 0, 5, 7, 21, 32, 35 |
  | `se252` | the worm's own combat set |
  | `se258` | bats, despite 258 being the worm family - so a sound folder is not a family |
  | `se006` | swords and walking |

  What is left is the effect. A worm surfacing throws up dirt, that dirt is a
  VFX, and an effect DAT carries sound references at its keyframes the same
  way these do. Finding which effect DAT is the emerge is the remaining step,
  and it is a search rather than a lookup.

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
