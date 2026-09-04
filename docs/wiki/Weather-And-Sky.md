# Weather and sky

A zone keeps a separate sky for each weather it can have, under `weat/<name>`,
and the client picks one when it builds the zone. The names are four
characters like everything else in a DAT.

## The vocabulary

Swept across the 265 zones that ship any weather directory at all:

| name | zones | reads as |
|---|---|---|
| `suny` | 229 | sunshine |
| `clod` | 211 | clouds |
| `mist` | 199 | fog |
| `fine` | 175 | clear |
| `thdr` | 59 | thunder |
| `rain` | 50 | rain |
| `wind` | 41 | wind |
| `dust` | 38 | dust storm |
| `fogd` | 33 | fog, a second kind |
| `dryw` | 32 | dry - hot spell or heat wave |
| `snow` | 24 | snow |
| `dark` | 24 | gloom or darkness |
| `squl` | 13 | squall |
| `heat` | 13 | heat |
| `aura` | 12 | auroras |
| `bliz` | 9 | blizzards |
| `stom` | 8 | storm |
| `bolt` | 7 | thunderstorms |
| `ligt`, `ligh` | 6, 1 | light - stellar glare |
| `sand` | 6 | sandstorm |

And a `1`-suffixed tier in a handful of zones each - `sun1`, `clo1`, `mis1`,
`thd1`, `win1`, `rai1`, `fog1`, `squ1`, `dus1`, `dry1`, `aur1` - which looks
like the double-strength half of FFXI's weather pairs, the Gales to Wind and
the Blizzards to Snow. Not confirmed.

## How many a zone ships

```
 1 sky:  38 zones      5 skies: 34        9 skies:  1
 2 skies: 4            6 skies: 61       10 skies:  4
 3 skies: 1            7 skies: 20       12 skies:  2
 4 skies: 93           8 skies:  7
```

The commonest arrangement by far is the plain four - `clod`, `fine`, `mist`,
`suny` - in 77 zones, then those four plus `rain` and `thdr` in 21. Eighteen
zones ship only `suny`, and thirteen ship only `dark`.

## What the client does with this

**Less than it could.** `skyForWeather` in `renderer/viewer.cpp` collapses the
server's twenty weathers onto the four common names, which is right for the 93
zones that ship exactly four and wrong everywhere else: a zone with `rain` and
`thdr` gets `mist` and `clod` instead. Its comment also claims auroras,
stellar glare and gloom "have no sky of their own", and the table above says
otherwise - `aura`, `ligt` and `dark` all ship.

The fix is to choose by name with a fallback chain, since a zone only has the
skies it has, and to pick from what the DAT actually contains rather than
assuming four. Not done yet.

Which weather number goes with which name is still read from what the words
mean rather than checked. `!weather <n>` on a LandSandBoat server sets it,
which makes that a quick thing to confirm.

## Weather also carries sound

See [Audio Formats](Audio-Formats.md): each `weat/<name>` directory declares
its own ambience, and a `weat/<name>/indo` beneath it holds an indoor variant.
In West Ronfaure all four skies name the same wind, so the choice is
inaudible there.
