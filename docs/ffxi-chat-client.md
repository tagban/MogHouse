# Idea: a standalone FFXI chat client

Parked for later. Everything this needs already works.

## Why it's viable now

PortJeuno can already do the whole chain without any rendering:

- log in (TLS+JSON auth), fetch the character roster, select a character
- connect to the zone server over UDP, encrypted and compressed
- hold the session open indefinitely with position heartbeats
- **send** `/say`, `/shout`, `/yell` (C2S `0x0B5`) and **tells** (C2S `0x0B6`)
- **receive** chat (S2C `0x017`) with sender name and message kind
- track who else is nearby (`0x00D`), by name

All of that is verified against a live server, and tells have been confirmed
arriving at a real retail client. The graphical half of the project — the
native engine, DAT assets, rendering — is not involved in any of it.

## What it would be

A text-only client for staying in touch with a linkshell or a party while away
from the game: a terminal or small GUI app that logs in, sits in a zone, and
relays chat. Cross-platform for free, since it's pure .NET with no engine
dependency.

## What's missing

- Linkshell chat and party chat plumbing (kinds exist in `FfxiChatKind`;
  the send path already supports them, the receive path needs the message
  types wired to a UI).
- Incoming tell display is parsed but not surfaced as a conversation.
- Reconnect handling for long unattended sessions.
- Some way to be a good citizen about idling in a zone.

## Where the code is

- `FfxiChatPacket` / `FfxiTellPacket` — sending
- `FfxiChatMessage` — receiving
- `FfxiZoneClient.HoldWithPositionAsync` — staying connected
- `FfxiEntityUpdate` — who else is around

See `docs/zone-protocol.md` for the transport details, including the two traps
that make chat easy to get subtly wrong (the length rule and the validated
"unknown" fields).
