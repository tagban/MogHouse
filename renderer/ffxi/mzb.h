#pragma once

// MZB - FFXI zone layout: where models are placed, and the collision geometry.
// Format established by reading the retail files; see docs/mzb-format.md.

#include "dat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ffxi
{
/// The 256-byte table MZB decryption needs. It comes from the retail client, so
/// it is deliberately not compiled in - load it from wherever the user keeps it.
class KeyTable
{
public:
    /// Reads 256 raw bytes. Returns nothing if the file is missing or the wrong
    /// size, so a caller can report that rather than decrypt with rubbish.
    static std::optional<KeyTable> load(const std::filesystem::path& path);

    uint8_t operator[](size_t index) const { return bytes_[index & 0xFF]; }

private:
    std::array<uint8_t, 256> bytes_{};
};

/// One placed model: which model, and where to put it.
struct Placement
{
    std::string model;
    float translate[3];
    float rotate[3]; // radians
    float scale[3];
};

/// One collision mesh, in model space.
struct CollisionMesh
{
    std::vector<float> vertices; // 3 floats per vertex
    std::vector<float> normals;  // 3 floats per normal
    std::vector<uint16_t> indices;
    uint16_t flags{};

    size_t vertexCount() const { return vertices.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
};

/// A parsed MZB chunk.
struct Zone
{
    std::string id;
    uint8_t version{};
    std::vector<Placement> placements;
    std::vector<CollisionMesh> collision;
};

/// Decrypts and parses one MZB chunk.
///
/// Throws std::runtime_error when the chunk does not make sense - a declared
/// length past the end of the data, or offsets outside it. Every retail zone
/// checked parses cleanly, so a throw here means either a new format variant or
/// the wrong key table.
Zone parseMzb(const Chunk& chunk, const KeyTable& keys);
} // namespace ffxi
