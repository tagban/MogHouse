// Reports what the C++ reader sees in a DAT, in the same shape as
// tools/mzbmesh.py, so the two can be compared. The python was checked against
// every zone in ROM/1; this exists so the C++ can be held to the same result.

#include "dat.h"
#include "lighting.h"
#include "mmb.h"
#include "mzb.h"
#include "os2.h"
#include "skeleton.h"
#include "texture.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
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
        std::printf("  key table path comes from MOGHOUSE_FFXI_KEYTABLE\n");
        return 2;
    }

    const char* keyPath = std::getenv("MOGHOUSE_FFXI_KEYTABLE");
    if (!keyPath)
    {
        std::printf("set MOGHOUSE_FFXI_KEYTABLE to a file holding the 256-byte key table\n");
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

        // What is actually in this file. Most types go unread, and knowing
        // which are present is the difference between "we do not draw the
        // fountain's flames" and "the flames are not in this file at all".
        if (std::getenv("MOGHOUSE_CHUNK_TALLY"))
        {
            std::map<uint8_t, size_t> tally;
            for (const ffxi::Chunk& chunk : dat.chunks())
            {
                ++tally[chunk.type];
            }
            std::printf("chunk types:");
            for (const auto& one : tally)
            {
                std::printf("  0x%02X x%zu", one.first, one.second);
            }
            std::printf("\n");
        }
        // Models. The check that matters is whether vertices land inside the
        // bounding box the model declares for itself - a wrong stride or offset
        // scatters them outside it immediately.
        // Every model the file holds, so the zone block below can say which
        // of them no placement ever names.
        std::set<std::string> modelNames;
        const char* key2Path = std::getenv("MOGHOUSE_FFXI_KEYTABLE2");
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
                    modelNames.insert(m.name);
                    // Per-model geometry, for finding a model that parses but comes out empty.
                    if (std::getenv("MOGHOUSE_MODEL_STATS"))
                    {
                        std::printf("  model %-18s %2zu meshes %6zu verts %6zu tris  tex %s\n",
                                    m.name.c_str(), m.meshes.size(), m.vertexCount(), m.triangleCount(),
                                    m.meshes.empty() ? "-" : m.meshes.front().texture.c_str());
                    }
                    if (const char* only = std::getenv("MOGHOUSE_MODEL_MESHES"))
                    {
                        if (m.name.rfind(only, 0) == 0)
                        {
                            std::printf("  %s: %zu meshes\n", m.name.c_str(), m.meshes.size());
                            for (const ffxi::ModelMesh& mesh : m.meshes)
                            {
                                std::printf("      %6zu tris  blend %u  tex [%s]\n",
                                            mesh.indices.size() / 3, unsigned(mesh.blending), mesh.texture.c_str());
                            }
                        }
                    }
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
            size_t textures = 0, bc1 = 0, bc2 = 0, paletted = 0, sizeMismatch = 0, failed = 0;
            std::string texSample, texFailure;
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
            {
                try
                {
                    ffxi::Texture t = ffxi::parseTexture(chunk);
                    ++textures;
                    if (t.format == ffxi::TextureFormat::Bc1)
                    {
                        ++bc1;
                    }
                    else
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
                std::printf("textures %zu (bc1 %zu, bc2 %zu, paletted %zu, failed %zu)\n", textures, bc1, bc2, paletted, failed);
                std::printf("  e.g. %s   size mismatches: %zu\n", texSample.c_str(), sizeMismatch);
                if (failed)
                {
                    std::printf("  first failure %s\n", texFailure.c_str());
                }
            }
        }

        // Skeletons. The checks: parents in range, the root pointing at
        // itself, and quaternions of unit length.
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
        {
            try
            {
                const ffxi::Skeleton skeleton = ffxi::parseSkeleton(chunk);
                size_t badParent = 0;
                size_t offUnit = 0;
                size_t roots = 0;
                for (size_t i = 0; i < skeleton.bones.size(); ++i)
                {
                    const ffxi::Bone& bone = skeleton.bones[i];
                    if (bone.parent >= skeleton.bones.size())
                    {
                        ++badParent;
                    }
                    if (bone.parent == i)
                    {
                        ++roots;
                    }
                    const float length = std::sqrt(bone.rotation[0] * bone.rotation[0] + bone.rotation[1] * bone.rotation[1] +
                                                   bone.rotation[2] * bone.rotation[2] + bone.rotation[3] * bone.rotation[3]);
                    if (std::fabs(length - 1.0f) > 0.02f)
                    {
                        ++offUnit;
                    }
                }
                std::printf("skeleton %.4s: %zu bones, %zu generator points\n", chunk.id, skeleton.bones.size(),
                            skeleton.generatorPoints.size());
                std::printf("  parents out of range: %zu   roots: %zu   non-unit rotations: %zu\n", badParent, roots,
                            offUnit);
            }
            catch (const std::exception& e)
            {
                std::printf("skeleton %.4s: %s\n", chunk.id, e.what());
            }
        }

        // Skinned meshes. The checks: every corner index inside the vertex
        // list, weights summing to one, and bones inside the skeleton that
        // shares the file.
        {
            size_t bones = 0;
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
            {
                try
                {
                    bones = std::max(bones, ffxi::parseSkeleton(chunk).bones.size());
                }
                catch (const std::exception&)
                {
                }
            }

            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkinnedMesh))
            {
                try
                {
                    const ffxi::SkinnedModel model = ffxi::parseOs2(chunk);
                    size_t badCorner = 0;
                    size_t badBone = 0;
                    size_t offWeight = 0;
                    std::string texture;
                    for (const ffxi::SkinnedPart& part : model.parts)
                    {
                        if (texture.empty())
                        {
                            texture = part.texture;
                        }
                        for (const ffxi::SkinCorner& corner : part.corners)
                        {
                            if (corner.vertex >= model.vertices.size())
                            {
                                ++badCorner;
                            }
                        }
                    }
                    for (const ffxi::SkinVertex& vertex : model.vertices)
                    {
                        float sum = 0.0f;
                        for (uint8_t i = 0; i < vertex.influences; ++i)
                        {
                            sum += vertex.influence[i].weight;
                            if (bones && vertex.influence[i].bone >= bones)
                            {
                                ++badBone;
                            }
                        }
                        if (std::fabs(sum - 1.0f) > 0.01f)
                        {
                            ++offWeight;
                        }
                    }
                    std::printf("skinned %.4s: %zu vertices, %zu triangles, %zu parts, tex %s%s\n", chunk.id,
                                model.vertices.size(), model.triangleCount(), model.parts.size(), texture.c_str(),
                                model.mirrored ? ", mirrored" : "");
                    std::printf("  corners out of range: %zu   bones out of range: %zu   weights off one: %zu\n",
                                badCorner, badBone, offWeight);
                }
                catch (const std::exception& e)
                {
                    std::printf("skinned %.4s: %s\n", chunk.id, e.what());
                }
            }
        }

        // Lighting, and whether it interpolates sensibly across the day.
        {
            ffxi::Lighting lighting;
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkLighting))
            {
                lighting.add(chunk);
            }
            if (!lighting.empty())
            {
                std::printf("lighting: %zu times of day\n", lighting.sets().size());
                for (const ffxi::LightingSet& set : lighting.sets())
                {
                    std::printf("  %02d:%02d  ambient %.2f %.2f %.2f  fog %.2f %.2f %.2f  maxfog %7.1f  bright %.2f\n",
                                set.minutes / 60, set.minutes % 60, set.landscapeAmbient.r, set.landscapeAmbient.g,
                                set.landscapeAmbient.b, set.landscapeFog.r, set.landscapeFog.g, set.landscapeFog.b,
                                set.landscapeMaxFog, set.landscapeBrightness);
                }
                // Interpolation should move smoothly rather than jumping.
                std::printf("  interpolated across the day:\n");
                for (int hour = 0; hour < 24; hour += 3)
                {
                    const ffxi::LightingSet set = lighting.at(hour * 60);
                    std::printf("    %02d:00  ambient %.2f %.2f %.2f  maxfog %7.1f\n", hour, set.landscapeAmbient.r,
                                set.landscapeAmbient.g, set.landscapeAmbient.b, set.landscapeMaxFog);
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
                size_t withWater = 0;
                float waterLo = 1e30f;
                float waterHi = -1e30f;
                for (const ffxi::CollisionInstance& instance : zone.instances)
                {
                    if (instance.waterHeight != 0.0f)
                    {
                        ++withWater;
                        waterLo = std::min(waterLo, instance.waterHeight);
                        waterHi = std::max(waterHi, instance.waterHeight);
                    }
                }
                if (withWater)
                {
                    std::printf("  water: %zu of %zu instances, heights %.1f to %.1f\n", withWater,
                                zone.instances.size(), waterLo, waterHi);
                }

                if (!zone.instances.empty())
                {
                    std::printf("  instance origins  x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n",
                                lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
                }
                std::printf("  %zu vertices, %zu triangles\n", vertices, triangles);
                // Five is enough to see the shape of the table; MOGHOUSE_LIST_PLACEMENTS
                // prints all of them, which is how you find out whether a zone
                // names its water meshes.
                const size_t listed = std::getenv("MOGHOUSE_LIST_PLACEMENTS") ? zone.placements.size() : 5;
                for (size_t i = 0; i < zone.placements.size() && i < listed; ++i)
                {
                    const ffxi::Placement& p = zone.placements[i];
                    std::printf("  %-16s %9.2f %8.2f %8.2f\n", p.model.c_str(), p.translate[0], p.translate[1], p.translate[2]);
                }

                // A model in the file that no placement names is either drawn
                // some other way or not drawn at all - and that gap is where
                // missing scenery would hide.
                std::set<std::string> placed;
                for (const ffxi::Placement& p : zone.placements)
                {
                    placed.insert(p.model);
                }
                std::vector<std::string> unplaced;
                for (const std::string& name : modelNames)
                {
                    if (!placed.count(name)) unplaced.push_back(name);
                }
                std::printf("  %zu distinct models placed, %zu models in the file, %zu never placed\n",
                            placed.size(), modelNames.size(), unplaced.size());
                for (size_t u = 0; u < unplaced.size(); ++u)
                {
                    std::printf("%s%s", u % 8 == 0 ? "    " : " ", unplaced[u].c_str());
                    if (u % 8 == 7 || u + 1 == unplaced.size()) std::printf("\n");
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
