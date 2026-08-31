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
