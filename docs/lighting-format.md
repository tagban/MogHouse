# Lighting, sky and time of day

Chunk type `0x2F`. **The chunk's four-character id is the time of day it applies
to** - `0000`, `0500`, `0600`, `1200`, `1700`, `1800`, `2100` - on FFXI's
1440-minute clock. A zone carries several chunks per time, presumably one per
weather type.

Not obfuscated. 176 bytes.

## Layout

| offset | type | field |
| --- | --- | --- |
| 0 | float[3] | unidentified, observed 0 |
| 12 | u32 | sunlight diffuse, entities |
| 16 | u32 | moonlight diffuse, entities |
| 20 | u32 | ambient, entities |
| 24 | u32 | fog, entities |
| 28 | float | max fog distance, entities |
| 32 | float | min fog distance, entities |
| 36 | float | brightness, entities |
| 44 | u32 | sunlight diffuse, landscape |
| 48 | u32 | moonlight diffuse, landscape |
| 52 | u32 | ambient, landscape |
| 56 | u32 | fog, landscape |
| 60 | float | max fog distance, landscape |
| 64 | float | min fog distance, landscape |
| 68 | float | brightness, landscape |
| 76 | u32 | fog colour |
| 80 | float | fog offset |
| 88 | float | maximum far clip |
| 100 | u32[8] | skybox colours |
| 132 | float[8] | skybox altitudes |

Entities and landscape are lit separately - characters and scenery do not share
a light.

## What the values look like

East Sarutabaruta, landscape figures:

| time | ambient | fog colour | max fog | brightness |
| --- | --- | --- | --- | --- |
| 0000 | 82, 84, 131 | 26, 27, 47 | 401.0 | 1.37 |
| 0500 | 112, 113, 122 | 78, 75, 71 | 401.0 | 1.50 |
| 0600 | 145, 143, 132 | 104, 96, 77 | 450.5 | 1.50 |
| 1200 | 166, 167, 174 | 111, 126, 145 | 450.5 | **1.70** |
| 1700 | 150, 131, 126 | 117, 94, 79 | 450.5 | 1.50 |
| 1800 | 138, 120, 117 | 82, 65, 54 | 450.5 | 1.50 |
| 2100 | 96, 100, 117 | 39, 41, 46 | **123.8** | 1.50 |

Midnight goes blue, noon is brightest, evening turns warm. And max fog at 2100
is 123.8 against noon's 450.5 - the night visibility drop is in the data, not a
renderer effect.

**Colour components are 0..128, not 0..255.** Alpha is 128 on every entry here,
which independently confirms the scaling that the texture alpha work ran into.

## Consequences for the renderer

The gradient sky currently in `sky_shader.h` is a placeholder for the eight-stop
`skybox_colors` and `skybox_values` ramp. Sky, fog and diffuse lighting all come
from this one structure, so they are one feature rather than three - and the
same structure is what makes a zone darker at night or indoors.

Blending between times is a straight interpolation on the clock: find the two
surrounding entries and mix.
