#pragma once

// Sprite animations - the type 0x21 chunks. A torch flame in FFXI is a
// camera-facing quad cycling through the frames of a sprite sheet; `hi12`'s
// runs sixteen frames over `hit3    hit32`, a 4x4 sheet of orange flame. The
// lamp glow `lt` is one frame over `effect  light`. Read 2026-09-03 from
// ROM/0/0.DAT and Bastok Markets; see docs/wiki/Effect-Generators.md.
//
// Layout from the chunk's start, sixteen-byte header included:
//
//   0x10  u8   1
//   0x11  u8   0
//   0x12  u8   frame count
//   0x13  u8   0
//   0x14  u8   0, then the frame count again twice, then 1
//   0x18  char[16]  texture, group then name
//   0x28  frames, each 148 bytes:
//           u32  1
//           6 vertices of 24 bytes: position xyz, colour rgba, uv
//
// The six vertices are two triangles making one quad in the plane z = 0,
// about two units tall with y running downward as everywhere in the DATs.
// Checked by (length - 0x28) / 148 equalling the count on every chunk seen.

#include "dat.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ffxi
{
struct SpriteVertex
{
    float position[3];
    uint32_t colour;
    float uv[2];
};

struct SpriteFrame
{
    std::array<SpriteVertex, 6> vertices;
};

struct SpriteAnimation
{
    std::string name;    ///< the chunk's four-character id
    std::string texture; ///< sixteen-byte texture field, as a mesh names one
    std::vector<SpriteFrame> frames;
};

std::optional<SpriteAnimation> parseSprite(const Chunk& chunk);

inline constexpr uint8_t kChunkSprite = 0x21;
} // namespace ffxi
