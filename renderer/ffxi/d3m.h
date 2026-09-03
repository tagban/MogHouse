#pragma once

// D3M - the small effect meshes of type 0x1f: torch and lamp flames, sparks,
// the fountain's fire. Read from ROM/0/0.DAT, the shared effects file that
// every zone's generators reach into for them (the zone DATs carry only an
// empty 80-byte MMB stub under the same id). See docs/wiki/Effect-Generators.md.
//
// Layout, from the chunk's own start (sixteen-byte header included):
//
//   0x10  u32  6              (constant in every one seen)
//   0x14  u16  1
//   0x16  u16  triangle count
//   0x18  u16  triangle count again
//   0x1a  u32  0
//   0x1e  char[16]  texture, as a mesh header names one: group then name
//   0x2e  3 * triangles vertices of 36 bytes: position xyz, normal xyz,
//         colour rgba, uv - the MMB vertex, as a plain triangle list
//
// hi12 - the fountain flame - is 24 triangles forming a tapered ribbon; most
// of the others are one quad. Checked by (length - 0x2e) / 36 coming out a
// whole number of triangles times three on all 45 models in the file.

#include "dat.h"
#include "mmb.h"

#include <optional>

namespace ffxi
{
/// Parses one type 0x1f chunk into a one-mesh Model named by the chunk's id.
/// Returns nothing if the chunk does not have the shape above.
std::optional<Model> parseD3m(const Chunk& chunk);

inline constexpr uint8_t kChunkD3m = 0x1f;
} // namespace ffxi
