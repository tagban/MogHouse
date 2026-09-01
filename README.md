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

## Controls

The world window takes the keyboard directly. Nothing here is configurable yet.

### Moving

| key | what it does |
|---|---|
| `W` `A` `S` `D` | walk and strafe |
| `R` | **auto-run** - keep going forward without holding the key. Press again, or press back, to stop. |
| `Shift` | run, or walk if you have toggled the default the other way |
| `numpad -` | swap which of walk and run is the default; `Shift` still inverts whichever it is |
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

### Talking and targeting

| key | what it does |
|---|---|
| `Return` | open the chat line. Only when there is a server to say it to - offline there is nothing to type into. |
| click | target whatever is under the pointer |

### Getting unstuck

Three keys exist because collision can trap you, and each fails differently.

| key | what it does |
|---|---|
| `U` | back up along the trail you walked in on. If that trail crossed somewhere that has no ground under it now, you will end up hanging there - `C` fixes that. |
| `N` | no collision. Walk through everything, including the floor. Press again to put it back. |
| `C` | place the character on the ground beneath them |
| `P` | print the position to the console |

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
