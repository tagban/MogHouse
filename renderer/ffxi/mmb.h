#pragma once

// MMB - FFXI models. Format read from the retail files; see docs/mmb-format.md.

#include "dat.h"
#include "mzb.h" // KeyTable

#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
struct ModelVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t colour;
};

/// One drawable piece of a model, with the texture it wants.
struct ModelMesh
{
    std::string texture;
    std::vector<ModelVertex> vertices;
    std::vector<uint16_t> indices; // always a triangle list, strips converted
    uint16_t blending{};
};

struct Model
{
    std::string group; ///< 8-byte tag shared across a zone's models
    std::string name;  ///< 16-byte name, matches an MZB placement's model id
    float boundsMin[3]{};
    float boundsMax[3]{};
    std::vector<ModelMesh> meshes;

    size_t vertexCount() const;
    size_t triangleCount() const;
};

/// Decrypts and parses one MMB chunk.
///
/// Throws std::runtime_error if the chunk does not make sense. Both key tables
/// come from the retail client, so KeyTable must be loaded from wherever the
/// user keeps them.
Model parseMmb(const Chunk& chunk, const KeyTable& keys, const KeyTable& keys2);
} // namespace ffxi
