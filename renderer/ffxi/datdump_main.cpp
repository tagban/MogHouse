// Reports what the C++ reader sees in a DAT, in the same shape as
// tools/mzbmesh.py, so the two can be compared. The python was checked against
// every zone in ROM/1; this exists so the C++ can be held to the same result.

#include "dat.h"
#include "mmb.h"
#include "mzb.h"
#include "texture.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-datdump <file.DAT> [more.DAT ...]\n");
        std::printf("  key table path comes from PORTJEUNO_FFXI_KEYTABLE\n");
        return 2;
    }

    const char* keyPath = std::getenv("PORTJEUNO_FFXI_KEYTABLE");
    if (!keyPath)
    {
        std::printf("set PORTJEUNO_FFXI_KEYTABLE to a file holding the 256-byte key table\n");
        return 2;
    }

    auto keys = ffxi::KeyTable::load(keyPath);
    if (!keys)
    {
        std::printf("could not read a 256-byte key table from %s\n", keyPath);
        return 2;
    }

    size_t totalVertices = 0;
    size_t totalTriangles = 0;
    size_t badIndices = 0;
    size_t nonUnitNormals = 0;
    size_t zones = 0;

    for (int arg = 1; arg < argc; ++arg)
    {
        ffxi::DatFile dat{std::filesystem::path{argv[arg]}};
        // Models. The check that matters is whether vertices land inside the
        // bounding box the model declares for itself - a wrong stride or offset
        // scatters them outside it immediately.
        const char* key2Path = std::getenv("PORTJEUNO_FFXI_KEYTABLE2");
        if (key2Path)
        {
            if (auto keys2 = ffxi::KeyTable::load(key2Path))
            {
                size_t models = 0, meshes = 0, verts = 0, tris = 0, outside = 0, badIndex = 0, offNormal = 0;
                std::string sample;
                size_t failed = 0;
                std::string firstFailure;
                for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMmb))
                {
                    ffxi::Model m;
                    try
                    {
                        m = ffxi::parseMmb(chunk, *keys, *keys2);
                    }
                    catch (const std::exception& e)
                    {
                        // One malformed model should not stop the survey - the
                        // point is to find out how many are wrong and why.
                        ++failed;
                        if (firstFailure.empty())
                        {
                            firstFailure = std::string(chunk.id, 4) + ": " + e.what();
                        }
                        continue;
                    }
                    ++models;
                    meshes += m.meshes.size();
                    verts += m.vertexCount();
                    tris += m.triangleCount();
                    if (sample.empty() && !m.meshes.empty())
                    {
                        sample = m.name + " tex=" + m.meshes.front().texture;
                    }
                    for (const ffxi::ModelMesh& mesh : m.meshes)
                    {
                        for (const ffxi::ModelVertex& v : mesh.vertices)
                        {
                            for (int a = 0; a < 3; ++a)
                            {
                                if (v.position[a] < m.boundsMin[a] - 0.5f || v.position[a] > m.boundsMax[a] + 0.5f)
                                {
                                    ++outside;
                                    break;
                                }
                            }
                            const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                                        v.normal[2] * v.normal[2]);
                            if (std::fabs(len - 1.0f) > 0.05f)
                            {
                                ++offNormal;
                            }
                        }
                        for (uint16_t index : mesh.indices)
                        {
                            if (index >= mesh.vertices.size())
                            {
                                ++badIndex;
                            }
                        }
                    }
                }
                if (models)
                {
                    std::printf("models %zu  meshes %zu  vertices %zu  triangles %zu\n", models, meshes, verts, tris);
                    std::printf("  e.g. %s\n", sample.c_str());
                    std::printf("  vertices outside their own bounds: %zu   bad indices: %zu   non-unit normals: %zu\n",
                                outside, badIndex, offNormal);
                    if (failed)
                    {
                        std::printf("  models that failed to parse: %zu, first %s\n", failed, firstFailure.c_str());
                    }
                }
            }
        }

        // Textures. BC2 stores one byte per pixel, so payload size should equal
        // width * height exactly - a cheap check that the header was read right.
        {
            size_t textures = 0, bc2 = 0, paletted = 0, sizeMismatch = 0, failed = 0;
            std::string texSample, texFailure;
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
            {
                try
                {
                    ffxi::Texture t = ffxi::parseTexture(chunk);
                    ++textures;
                    if (t.format == ffxi::TextureFormat::Bc2)
                    {
                        ++bc2;
                        if (t.pixels.size() != static_cast<size_t>(t.width) * t.height)
                        {
                            ++sizeMismatch;
                        }
                    }
                    else
                    {
                        ++paletted;
                    }
                    if (texSample.empty())
                    {
                        texSample = t.name + " " + std::to_string(t.width) + "x" + std::to_string(t.height);
                    }
                }
                catch (const std::exception& e)
                {
                    ++failed;
                    if (texFailure.empty())
                    {
                        texFailure = std::string(chunk.id, 4) + ": " + e.what();
                    }
                }
            }
            if (textures || failed)
            {
                std::printf("textures %zu (bc2 %zu, paletted %zu, failed %zu)\n", textures, bc2, paletted, failed);
                std::printf("  e.g. %s   size mismatches: %zu\n", texSample.c_str(), sizeMismatch);
                if (failed)
                {
                    std::printf("  first failure %s\n", texFailure.c_str());
                }
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
        {
            ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);
            ++zones;

            size_t vertices = 0;
            size_t triangles = 0;
            for (const ffxi::CollisionMesh& mesh : zone.collision)
            {
                vertices += mesh.vertexCount();
                triangles += mesh.triangleCount();

                for (uint16_t index : mesh.indices)
                {
                    if (index >= mesh.vertexCount())
                    {
                        ++badIndices;
                    }
                }
                for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3)
                {
                    const float x = mesh.normals[i];
                    const float y = mesh.normals[i + 1];
                    const float z = mesh.normals[i + 2];
                    if (std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) > 0.02f)
                    {
                        ++nonUnitNormals;
                    }
                }
            }

            if (argc == 2)
            {
                std::printf("zone %s  version=%#04x  placements=%zu  meshes=%zu  instances=%zu\n", zone.id.c_str(), zone.version,
                            zone.placements.size(), zone.collision.size(), zone.instances.size());

                float lo[3] = {1e30f, 1e30f, 1e30f};
                float hi[3] = {-1e30f, -1e30f, -1e30f};
                for (const ffxi::CollisionInstance& instance : zone.instances)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        lo[axis] = std::min(lo[axis], instance.transform[12 + axis]);
                        hi[axis] = std::max(hi[axis], instance.transform[12 + axis]);
                    }
                }
                if (!zone.instances.empty())
                {
                    std::printf("  instance origins  x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n",
                                lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
                }
                std::printf("  %zu vertices, %zu triangles\n", vertices, triangles);
                for (size_t i = 0; i < zone.placements.size() && i < 5; ++i)
                {
                    const ffxi::Placement& p = zone.placements[i];
                    std::printf("  %-16s %9.2f %8.2f %8.2f\n", p.model.c_str(), p.translate[0], p.translate[1], p.translate[2]);
                }
            }

            totalVertices += vertices;
            totalTriangles += triangles;
        }
    }

    std::printf("\n%zu zones, %zu vertices, %zu triangles\n", zones, totalVertices, totalTriangles);
    std::printf("indices out of range: %zu    non-unit normals: %zu\n", badIndices, nonUnitNormals);
    return (badIndices || nonUnitNormals) ? 1 : 0;
}
