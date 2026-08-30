// Skins a character out of a DAT and reports what came out, without a GPU.
//
// The checks that matter are geometric: a humanoid should be a little under
// two units tall, roughly symmetric about x, and standing on the origin. A
// wrong bone chain shows up as a body scattered over tens of units long before
// it is worth putting on screen.

#include "character.h"
#include "ffxi/dat.h"
#include "ffxi/os2.h"
#include "ffxi/skeleton.h"
#include "ffxi/texture.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-chardump <file.DAT> [more.DAT ...]\n");
        return 2;
    }

    for (int arg = 1; arg < argc; ++arg)
    {
        std::printf("=== %s ===\n", argv[arg]);
        ffxi::DatFile dat{std::filesystem::path{argv[arg]}};

        ffxi::Skeleton skeleton;
        bool haveSkeleton = false;
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
        {
            try
            {
                skeleton = ffxi::parseSkeleton(chunk);
                haveSkeleton = true;
                std::printf("skeleton %.4s: %zu bones\n", chunk.id, skeleton.bones.size());
                break;
            }
            catch (const std::exception& e)
            {
                std::printf("skeleton %.4s: %s\n", chunk.id, e.what());
            }
        }
        if (!haveSkeleton)
        {
            std::printf("no skeleton in this file\n");
            continue;
        }

        std::unordered_map<std::string, ffxi::Texture> textures;
        for (const ffxi::Chunk& chunk : dat.chunksOfType(0x20))
        {
            try
            {
                ffxi::Texture texture = ffxi::parseTexture(chunk);
                textures.emplace(texture.name, std::move(texture));
            }
            catch (const std::exception&)
            {
            }
        }

        std::vector<ffxi::SkinnedModel> meshes;
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkinnedMesh))
        {
            try
            {
                ffxi::SkinnedModel model = ffxi::parseOs2(chunk);
                if (!model.parts.empty())
                {
                    meshes.push_back(std::move(model));
                }
            }
            catch (const std::exception& e)
            {
                std::printf("skinned %.4s: %s\n", chunk.id, e.what());
            }
        }

        const std::vector<pj::BonePose> pose = pj::bindPose(skeleton);

        // How far the skeleton itself reaches. If this is wrong the mesh has
        // no chance, and it is far easier to read.
        pj::Vec3 low{1e9f, 1e9f, 1e9f};
        pj::Vec3 high{-1e9f, -1e9f, -1e9f};
        for (const pj::BonePose& bone : pose)
        {
            low = {std::fmin(low.x, bone.translation.x), std::fmin(low.y, bone.translation.y),
                   std::fmin(low.z, bone.translation.z)};
            high = {std::fmax(high.x, bone.translation.x), std::fmax(high.y, bone.translation.y),
                    std::fmax(high.z, bone.translation.z)};
        }
        std::printf("bones span x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f\n", low.x, high.x, low.y, high.y, low.z,
                    high.z);

        const pj::Character character = pj::buildCharacter(pose, meshes, textures);
        std::printf("skinned %zu meshes -> %zu vertices, %zu triangles, %zu batches\n", meshes.size(),
                    character.vertices.size(), character.triangles(), character.batches.size());
        std::printf("bounds  x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f   height %.3f\n", character.boundsMin.x,
                    character.boundsMax.x, character.boundsMin.y, character.boundsMax.y, character.boundsMin.z,
                    character.boundsMax.z, character.height());

        for (const pj::Batch& batch : character.batches)
        {
            const auto found = textures.find(batch.texture);
            std::printf("  batch %-18s %6u indices  %s%s\n", batch.texture.c_str(), batch.indexCount,
                        batch.cutout ? "cutout" : "opaque", found == textures.end() ? "  (texture not in this file)" : "");
        }
    }
    return 0;
}
