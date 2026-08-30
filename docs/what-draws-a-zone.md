# What actually draws a zone

Rendering every MZB placement leaves a lot of black. This is where the rest of
the surfaces come from, measured rather than guessed - the counts below are East
Sarutabaruta.

Of **46 textures** in the zone's DAT:

| | count | drawn by |
| --- | --- | --- |
| used by placed models | 22 | the MZB placement table - what we render today |
| used only by models the MZB never places | 17 | something else |
| referenced by no model at all | 7 | not geometry |

## The 17: water and effects

Those textures sit on models that exist in the DAT but appear nowhere in the
placement table. Their names, from the Japanese:

| name | meaning |
| --- | --- |
| `effect ike1`, `ike2` | pond |
| `effect kaw1` | river |
| `effect nami` | wave |
| `effect tak1` | waterfall |
| `effect sakf/sakk/sakm` | cherry blossom |
| `kamome kamome01` | seagull |

So water is not part of the placed geometry. Two things point at where it does
come from. MZB's grid entries carry a **`water_height`** field 164 bytes into
each placement record, which we parse past and discard. And the DAT holds 425
chunks of type `0x05` (generators) and 7 of type `0x07` (schedulers) - FFXI's
effect scripting - which is the obvious candidate for animated surfaces and
wildlife.

## The 7: sky

`clod_a01` (cloud), `moon moonshap`, `moon kasa`, and `lf01` through `lf03`
(lens flare). **No model references these**, because the sky is not geometry -
the engine draws it. MZB's lighting data carries a skybox colour ramp, eight
altitudes and eight colours, which is what fills the space above the horizon.

No amount of work on placed models will fill that, which is worth knowing before
spending any.

## What this means for the renderer

Three separate sources, and we implement one:

1. **Placed models** - done
2. **Sky** - a colour ramp and a few celestial sprites, cheap, and it fills the
   largest single area of black
3. **Water and effects** - driven by the generator and scheduler chunks, which
   are their own format and their own project

Sky first, on the grounds that it is the least work for the most coverage.
