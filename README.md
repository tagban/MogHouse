# MogHouse

A faithful attempt to port over FFXI game client to work more universally with other operating systems for private server usage. Open source so that if SE ever wants to update their client they can HAVE it..

The client reads the retail install's own DAT files directly - zones, models,
skeletons, animations, textures, entity names - and speaks the FFXI protocol to
a private server. Nothing here ships game assets.

## Building

`build-renderer.bat` on Windows, `build-renderer.sh` elsewhere, then
`dotnet build MogHouse.slnx`. See [docs](docs) for what has been worked out
about the file formats so far.

## Running

    .\run.ps1 -ZoneId 235 -Look 1,0,0,1,1,1,1

opens a zone with a character standing in it, no server needed. To play against
a LandSandBoat server, `MogHouse.Console login --help` lists what it takes.

## Tests

```
MOGHOUSE_FFXI_RES=/path/to/LandSandBoat/res dotnet test src/MogHouse.Core.Tests
```

Twelve of them need the retail compression tables, which are not bundled, and
skip themselves with an explanation if `MOGHOUSE_FFXI_RES` is not set. All 143
pass with it.

## Controls

The world window takes the keyboard directly. Nothing here is configurable yet.

Where a key exists twice on a keyboard this says which one is meant, because
two of them do different things: `numpad -` swaps walk and run, and the `-` on
the number row turns the music down.

### Moving

| key | what it does |
|---|---|
| `W` `A` `S` `D` | walk and strafe |
| `R` | **auto-run** - keep going forward without holding the key. Press again, or press back, to stop. |
| `Shift` | run, or walk if you have swapped the default |
| `numpad -` | swap which of walk and run is the default. `Shift` still inverts whichever it is. |
| `numpad 8` `2` | forward and back |
| `numpad 4` `6` | turn |
| `Space` | jump. Lying down it waves instead, which is the only thing a corpse can do to be noticed. |

### Looking

| key | what it does |
|---|---|
| mouse drag | turn the camera |
| wheel, or `numpad 9` `3` | zoom |
| `Tab` | orbit |
| `F` | swap between driving the character and flying the camera |
| `M` | swap the minimap between turning with you and holding north up |

The minimap starts turning with you. `MOGHOUSE_RADAR_NORTH` starts it the other
way. You are the orange dot in the middle, other players are blue, everything
else is green.

### Sound

| key | what it does |
|---|---|
| `-` (number row) | music quieter, 5% a press |
| `+` or `=` (number row) | music louder |

**The number row, not the numpad** - `numpad -` swaps walk and run. `=` works as
well as `+` because nobody holds shift to turn the music up.

Music starts at 35%. `MOGHOUSE_MUSIC_VOLUME` sets that, 0 to 1.

### Talking and targeting

| key | what it does |
|---|---|
| `Return` | open the chat line. Only when there is a server to say it to - offline there is nothing to type into. |
| `/` | open the chat line with `/` already in it, for the client's own commands |
| `!` | open the chat line with `!` already in it, for the server's GM commands |
| click | target whatever is under the pointer |

A link in the chat log is clickable, and hovering says where it goes. The two
chips in the top left corner open the Discord and the issue tracker.

### Chat channels

Typed into the chat line. Every one has the short form the real client takes,
because nobody types `/linkshell` twice.

| command | short | goes to |
|---|---|---|
| `/say` | `/s` | everyone nearby |
| `/shout` | `/sh` | the zone |
| `/yell` | `/y` | further than that |
| `/party` | `/p` | your party |
| `/linkshell` | `/l`, `/ls` | your linkshell |
| `/linkshell2` | `/l2`, `/ls2` | the second one |
| `/unity` | `/u` | your unity |
| `/emote` | `/em`, `/me` | an emote |
| `/tell <name> <what>` | `/t`, `/w` | one person |

And the client's own: `/logout`, `/shutdown` (`/quit`), `/homepoint` (`/hp`,
`/return`). Anything else beginning with `/` says so rather than being shouted
across the zone, which is what used to happen.

### Getting unstuck

Four keys exist because collision can trap you, and each fails differently.

| key | what it does |
|---|---|
| `U` | back up along the trail you walked in on. If that trail crossed somewhere with no ground under it now, you will hang there - `C` fixes that. |
| `N` | no collision. Walk through everything, including the floor. Press again to put it back. |
| `C` | put the character back on the ground |
| `P` | print the position to the console |

Falling through the floor recovers on its own now, but `C` is still the quick
answer to being somewhere you should not be.

`Escape` quits.

### On the server side

These are LandSandBoat's GM commands, typed into the chat line, not ours.

| command | what it does |
|---|---|
| `!pos` | print your position, zone and the terrain you are standing on |
| `!pos <x> <y> <z> <zone>` | go somewhere. `!zone` wants an auto-translate name and will not take a plain one; this takes a zone id. Windurst Waters is 238, Walls 239, Port 240, Woods 241. |
| `!wallhack` | walk through the server's walls. It is a flag on the character and it persists across logins, so it stays on until you turn it off. |
| `!godmode` | stop taking damage |
| `!up` / `!down` | move yourself vertically |
