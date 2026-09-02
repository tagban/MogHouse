# The session goes quiet after zone-in

For the Windows session. Written on the Mac on 2026-09-02, against a local
LandSandBoat built for the purpose.

Login and zone-in work. What does not is everything after: no entity updates
arrive, the character cannot be seen to move by the server, and the server
reaps the session about a minute later. This is almost certainly not a macOS
problem, and you have the one thing this machine does not - a client that
demonstrably walks around on a live server.

## What works

Verified end to end, on macOS, with the renderer on the main thread:

    logged in - 16 character(s)
    entering the world as Testy...
    in zone 234 at -45.0 0.0 25.0
    opening the world on thread 1

and the world drawn: Bastok Mines, 223 models, 13 building interiors, 103791
triangles of collision, 1477 draws, 53 textures. The character stands in it
with their nameplate over their head.

So the auth server, the lobby, character creation, the zone handoff, the zone
login reply, the DAT reading and the whole renderer are all fine.

## What does not

The client sends position every 400ms - `HoldWithPositionAsync` with a
`positionProvider`, `duration: 12 hours`, `interval: 400ms` - and receives
nothing back, forever:

    zone: nothing arrived within 0.4s
    zone: nothing arrived within 0.4s      (repeating)

Meanwhile the server, which accepted the login two seconds earlier, quietly
stops answering and then drops the session:

    07:09:03  Creating session for 127.0.0.1
    07:09:03  Player <Testy> logging in to zone <234>
    07:09:05  Bastok_Mines IncreaseZoneCounter <1> Testy
    07:10:11  Clearing map server session for player: 'Testy'
    07:10:11  Bastok_Mines DecreaseZoneCounter <0> Testy

68 seconds is the ~60s reap after the last *valid* packet. So the packets are
arriving and being rejected rather than not arriving: this protocol drops what
it does not like without saying so, which is why there is nothing in the map
log about it.

## The server's side of it, traced

Since the server is local, its own code answers most of this.

`cleanupSessions` does not care about movement. It keys on `last_update`:

    if (now > map_session_data->last_update + 5s)                    // mark link-dead
    if (now > map_session_data->last_update + MAX_TIME_LASTUPDATE)   // clear the session

with `MAX_TIME_LASTUPDATE = 60`, which is the 68s we measured. A player standing
perfectly still keeps their session alive, so an unchanging position is not why
this dies.

`last_update` is refreshed in exactly one place, at the top of
`MapNetworking::parse`:

    if (PSession->blowfish.status != BLOWFISH_PENDING_ZONE &&
        PSession->blowfish.status != BLOWFISH_WAITING)
    {
        PSession->tapLastUpdate();   // "Update the time we last got a char sync packet"
    }

The `0x00A` we sent did reach the server and did its job - the map log shows the
zone-in - which sets `BLOWFISH_ACCEPTED`. So the status gate is open, and any
later packet reaching `parse` would refresh the timer.

It never was refreshed. **So the packets after login are not reaching `parse` at
all** - they are being dropped in decryption, before any handler runs. That is
consistent with the client receiving nothing back: the server only answers
packets it accepted.

## The hypothesis, and why it is only that

The login packet is accepted and the reply decodes - so at that moment the
Blowfish key, the counters and the checksum are all right. Everything sent
afterwards is refused. Whatever differs between the first packet and the rest
is the bug.

`FfxiZoneClient.TryAdvanceKey` is the obvious suspect, and its own comment is
the reason:

> A failed checksum here is the only signal that the server rotated its key.

That signal only ever arrives inside a reply. If the server rotates its key
at or just after zone-in, the client cannot learn it, because it never receives
another packet to fail a checksum on. That is a deadlock rather than a
mistake in the rotation logic, and it would look exactly like this.

The server's own comment in the `0x00A` handler says as much:

> Key is already assumed to be incremented correctly

So it expects the client's key to have advanced by the time the zone-in
completes. If the client is still on the previous key, every packet it sends
fails decryption, never reaches `parse`, never taps the timer - and the server,
having nothing valid to answer, sends nothing, so the client never gets the
failed checksum that is its only cue to advance. Each side is waiting for the
other.

If that is right, the fix is for the client to advance its key at zone-in
rather than reactively, and `TryAdvanceKey` stays only as a recovery path.

**Treat that as unconfirmed.** Every measurement fits it and the server comment
supports it, but nothing here proves the client is on the wrong key - only that
its packets are refused before `parse`. Instrumenting the server side, or
comparing the key state against a Windows client that works, would settle it.

## The thing you can settle and I cannot

**Does this reproduce against a real server?** Your 0.1.2 build walks around
ffxi.cc, so the protocol code is right in production. That leaves two
possibilities and they need different work:

1. **It reproduces on ffxi.cc too**, from `tools/loginzone`. Then it is a real
   client bug that the normal client avoids by doing something the harness does
   not - and the difference between `GameViewModel` and the harness is the
   place to look, not the protocol.
2. **It does not reproduce**, and only the local server sees it. Then it is
   this LandSandBoat build or its configuration, and the fix is on the server
   side or in what the client assumes about it.

Running `tools/loginzone` against ffxi.cc takes a minute and decides which
half of the problem this is. It needs three environment variables and stores
nothing:

    MOGHOUSE_TEST_HOST=ffxi.cc
    MOGHOUSE_TEST_USER=...
    MOGHOUSE_TEST_PASS=...
    MOGHOUSE_FFXI_RES=<dir with compress.dat and decompress.dat>
    dotnet run --project tools/loginzone

Watch for `zone: nothing arrived within 0.4s`. If the world opens and entity
nameplates appear around you, it is case 2.

## What was already ruled out

- **Not a version mismatch.** I chased this and was wrong: the game client has
  not changed since August, so LandSandBoat commits since then are server-side
  gameplay and do not move packet layouts.
- **Not the compression tables.** They are needed - without them the zone
  reply decrypts, passes its checksum and then decodes to nothing - but they
  are configured here and the zone reply parses.
- **Not the renderer, Metal, WGSL or Dawn.** The world draws correctly.
- **Not the entity feed alone.** The harness deliberately does not call
  `set_entities`, so entity nameplates would be missing regardless - but the
  session receiving nothing is upstream of that and is the real problem.

## Two diagnostics added along the way

`FfxiZoneClient.ReceiveAsync` now says which of the two failure modes happened,
because they were indistinguishable and mean opposite things:

    zone: nothing arrived within 2s
    zone: 484 bytes from 127.0.0.1:54230, checksum ok, declared bits 3486, no plaintext

The first says look at the network. The second says look at the key or the
codec. Both used to surface as "Zone server did not answer, or its reply could
not be decoded", which is a poor sentence to debug from and cost real time here.

## Reproducing the whole thing on this Mac

`docs/local-test-server.md` has the LandSandBoat build, which is not obvious -
Apple's clang has no `std::jthread`, Homebrew's current LLVM is too new,
LLVM 20 is the one that works, and its libc++ has to be linked rather than only
its headers used. The servers are built and the database is populated; starting
them is:

    cd /Volumes/AppStorage/LandSandBoat
    ./xi_world & ./xi_map & ./xi_connect &

with an account already made: `mhtest` / `mhtestpw123`, one character, `Testy`.
