# The FFXI zone/map protocol, as implemented here

Everything below was established by reading LandSandBoat's server source and
then confirming it against a live server — mostly by compiling the real C++
structs and functions standalone with MSVC and testing this port's output
against theirs. Where something is inferred rather than confirmed, it says so.

The three transports in a login are unrelated to each other:

| Stage | Port | Transport |
|---|---|---|
| Auth | 54231 | TLS + JSON |
| Data / view (roster, character select) | 54230 / 54001 | Plain TCP |
| Zone / map | 54230 | **UDP**, Blowfish-encrypted, Huffman-compressed |

The zone port collides numerically with the data port but is a different
protocol on a different socket type.

## Datagram framing

```
+--------------------+----------------------+------------------+
| 28-byte header     | body                 | 16-byte MD5      |
+--------------------+----------------------+------------------+
```

Only three header fields are ever read:

- offset 0 (`uint16`) — the sender's own packet counter
- offset 2 (`uint16`) — the highest counter seen from the peer
- offset 8 (`uint32`) — a timestamp

Everything else stays zero. Both directions use the same shape, with the roles
of offsets 0 and 2 swapping by perspective.

The MD5 covers the body only, never the header. Critically, it is computed over
the **decrypted** bytes, which makes it double as the "did this decrypt?" test —
that is exactly how the server decides whether to retry with a previous key, and
it is why a passing checksum proves the cipher, key schedule and key derivation
are all correct at once.

## Sub-packets

The body is a chain of sub-packets, each opening with a `uint16` that packs
`id:9` and `size:7`, where the size is in **4-byte units**. The server reads the
id as `word & 0x1FF` and recovers the size as `(byte1 & 0xFE) * 2`.

Bitfield bit-order is implementation-defined, so this was read back from the
real compiled struct rather than derived: `id=0x00A, size=23` lays out as
`0x2E0A`.

Iteration stops at the first entry declaring a zero or over-long size — the tail
of a payload is often padding, not a packet.

## Blowfish

Not textbook Blowfish. The F-function masks two of its four S-box lookups down
to their low bit and XORs with `0x20` before adding, where standard Blowfish
uses the raw S-box word. A stock library silently produces wrong ciphertext.

The key comes from `accounts_sessions.session_key`, MD5-hashed. **The client
does not need to read that from anywhere** — it originates the value itself, as
the `key3` blob in its own `0xA2` character-select request, which the login
server merely stores. Because `key3` starts at packet offset 1 and the mandatory
session-hash region starts at offset 12, `key3` is necessarily 11 zero bytes
followed by the first 9 bytes of the session hash.

Two non-obvious details in the key schedule, both of which would break nearly
every real connection and neither of which fails loudly:

1. When the MD5 hash contains a zero byte, the key is **zero-padded to a full
   16 bytes** — not shortened. The cyclic XOR modulus stays 16.
2. `blowfish_init` takes the key as `const int8[]` — **signed**. Any byte
   `>= 0x80` sign-extends and clobbers the upper bytes of the accumulator. This
   looks like a bug in the server, but it has to be replicated bit-for-bit.
   About 96% of random 16-byte keys contain such a byte, so this is the common
   case, not an edge case.

The server rotates its key (`key[4] += 2`, re-derive) every time it sends a
`0x00B`. The rotation is invisible: afterwards the `0x00B` announcing it is
itself undecryptable, so the only evidence is that both directions stop working
at once. `FfxiZoneClient` probes forward on a checksum failure, mirroring the
server's own `prev_blowfish` fallback. *This has not yet been observed firing.*

## Compression

What the server calls zlib is **not zlib**, despite living in `zlib.cpp`. It is a
static Huffman codec driven by two fixed lookup tables loaded at startup from
`res/compress.dat` (2KB) and `res/decompress.dat` (10KB).

- **Sizes are in bits, not bytes.** The on-wire size field stores a bit count;
  `zlib_compressed_size` is `(bits + 7) / 8`.
- Compressed buffers open with a literal `0x01` marker byte, with the bitstream
  from offset 1 — hence the `+8` / `-8` around lengths.
- The server passes its declared size to its own decompressor unadjusted even
  though that function measures from *after* the marker, so it walks 8 bits past
  the end and decodes a stray trailing symbol. Harmless there, because
  sub-packet iteration stops before reaching it. This port subtracts the 8.

The tables are **not** bundled with PortJeuno: they are GPLv3 in the LandSandBoat
repo and their contents most likely originate in the retail client, so shipping
them is a licensing decision rather than something to make by accident. They are
loaded from a path (`PORTJEUNO_FFXI_RES`, or beside the executable). Their bytes
do not appear in any retail client DLL in either 32- or 16-bit packing, so they
appear to be built at runtime rather than stored.

## Handshake

`GP_CLI_COMMAND_LOGIN` (`0x00A`, 92 bytes) is the first packet a client sends
and the only one ever sent in the clear — the server tries a plaintext MD5 first
and requires anything validating that way to be an `0x00A`.

What the server actually checks: the outer MD5; the packet id; a minimum length;
a one-byte sum over everything from offset 8 to the end; and that `UniqueNo`
names a charid with a *pending session*, which the login server creates by IPC
during character select. `Ticket`, `sName`, `sAccunt`, `GrapIDTbl` and
`sPlatform` are never read at all.

**Retransmission is structurally required, not just good UDP hygiene.** A first
`0x00A` from an unknown address is answered by nothing, for two independent
reasons:

1. `handle_incoming_packet` passes its null session pointer to `recv_parse`
   **by value**. `recv_parse` does create the session, but assigns it to its own
   local parameter — so the caller's pointer is still null on return, and the
   next line is `if (PSession == nullptr) return;`. The session now exists;
   nothing was sent back.
2. The pending session it consumes is created by an IPC hop (login → world →
   map) that races the handoff packet. A fast client wins.

Both clear on the second attempt.

## Staying alive

`GP_CLI_COMMAND_POS` (`0x015`, 32 bytes) is the real heartbeat. Its handler sets
`UPDATE_POS` and `requestedInfoSync`, which is what pushes the character's state
to other players. A client that never sends it logs in successfully and then
renders to others as "timed out" — accurately, since nothing is arriving.

**The three floats are not x/y/z in order.** The server's own handler maps them
with a `// Not a typo.` comment: the second float is the engine's *vertical*
axis, the third is horizontal depth. Getting this wrong does not fail loudly —
it buries the character in terrain. Verified end-to-end: one client walking a
3-unit circle was observed by another at the correct radius on x and z with the
vertical held exactly constant.

Sessions are reaped roughly 60 seconds after the last valid packet.

## Chat

- C2S `0x0B5` — `Kind` at 4, `unknown00` at 5, `Str` at 6.
- S2C `0x017` — `Kind` at 4, `Attr` at 5, `Data` (`uint16`) at 6, sender at 8,
  text at 23. The `uint16` in the middle is what moves the later fields off
  where a by-hand reading would put them.

The send side is variable-length and **the server never looks for a NUL
terminator** — it computes the message length from the declared size as
`header.size * 4 - 6`, with its own source warning that the message may not be
terminated. Sizes count in 4-byte units, so packets are padded up, and that
padding lands *inside* the range the server reads. Pad with NULs.

`FfxiChatKind` (what a client sends) and `FfxiChatMessageType` (what the server
sends) are separate enumerations. They overlap on Say/Shout/Party by
coincidence; the server's list continues into tells and system messages no
client ever sends. Do not cast between them.

## Entity updates

`0x00D` (another player) and `0x00E` (NPC/mob) both open with the same
`GP_SERV_POS_HEAD` block that `GP_SERV_COMMAND_LOGIN` uses, so identity and
position are read the same way for all three.

An `0x00D` arriving is **not** evidence another player is visible — its
`UniqueNo` is usually the receiving character's own id, because the server
describes you to yourself first. Compare against your own charid before
concluding anything.

## Server-side gotchas worth knowing

- `accounts_sessions` has `UNIQUE KEY (accid)`. Two characters on one account
  can never be online simultaneously. Error **305**
  (`UNABLE_TO_CONNECT_TO_WORLD_SERVER`) means the session insert failed, which
  is that constraint or a stale row far more often than the world server
  actually being down.
- Error **201** means a session row for that character still exists. Both are
  routine when re-running tests inside the ~60s cleanup window — wait it out.
- Error packets are command `0x24`, 36 bytes, with `0x04` in the result field at
  offset 8 and the code as a **`uint16` at offset 32**.
- `SaveCharGMLevel` writes the character's *in-memory* GM level back to the
  database on logout and on zoning, so editing `chars.gmlevel` while the
  character is logged in gets silently overwritten. Edit it while logged out.
