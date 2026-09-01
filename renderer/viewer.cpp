// MogHouse's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
// Windows, Vulkan on Linux - and draws FFXI zone geometry read from the retail
// DATs.
//
// Given a DAT it draws that zone's collision geometry. Given nothing it only
// clears, so the graphics path can still be checked on a machine with no game
// installed.

#include "ffxi/dat.h"
#include "ffxi/filetable.h"
#include "ffxi/look.h"
#include "ffxi/mmb.h"
#include "ffxi/lighting.h"
#include "ffxi/mo2.h"
#include "ffxi/entitynames.h"
#include "ffxi/mzb.h"
#include "ffxi/os2.h"
#include "ffxi/skeleton.h"
#include "ffxi/texture.h"
#include "gputexture.h"
#include "camera.h"
#include "character.h"
#include "collision.h"
#include "viewer.h"

#include <deque>
#include "coverage.h"
#include "linalg.h"
#include "chat_shader.h"
#include "dialog_shader.h"
#include "hud_shader.h"
#include "nameplate_shader.h"
#include "zoneline_shader.h"
#include "textfont.h"
#include "radar_shader.h"
#include "scene.h"
#include "surface.h"
#include "sky_shader.h"
#include "music.h"
#include "water_shader.h"
#include "zone_shader.h"
#include "zonemesh.h"

#include <SDL3/SDL.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <fstream>
#include <sstream>

namespace
{
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth24Plus;

struct Uniforms
{
    float viewProjection[16];
    float lightDirection[4];
    float ambient[4];
    float sunlight[4];
    float fogColour[4];
    float fogRange[4];
    float eye[4];
};

/// Matches RadarUniforms in radar_shader.h.
/// Index into the 4x6 font's character set, or 0 - a space - for anything it
/// does not have. Lower case folds to upper, because the font has no lower.
///
/// A missing glyph becomes a space rather than a wrong letter: a gap reads as
/// "this font cannot show that", where a substitution reads as a bug in
/// whatever produced the text.
inline int glyphIndex(char raw)
{
    static const std::string kOrder = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-.";
    const char upper = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
    const size_t found = kOrder.find(upper == '_' ? ' ' : upper);
    return found == std::string::npos ? 0 : static_cast<int>(found);
}

/// A character cell in the chat panel, wide over tall. The glyph is 4x6 with
/// a column of space beside it and two rows above and below.
inline constexpr float kGlyphAspect = 5.0f / 8.0f;

/// Name colours, by what the thing is.
///
/// The full list a player would expect - party, linkshell, friend, pet - needs
/// membership this client does not read from any packet yet. Rather than guess
/// at those, only the three that can actually be told apart are coloured.
inline constexpr float kHudBright[3] = {0.97f, 0.97f, 1.00f};
inline constexpr float kHudDim[3] = {0.78f, 0.82f, 0.90f};

inline constexpr float kNameWhite[3] = {0.98f, 0.98f, 1.00f};
inline constexpr float kNameNpc[3] = {0.60f, 0.98f, 0.60f};

/// How far above the top of a character its name sits. Added to the model's
/// own height rather than used as one: a fixed height tuned for a hume left a
/// tarutaru's name floating a whole body length over its head, because a taru
/// is 0.94 tall against a hume's 1.79.
inline constexpr float kPlateClearance = 0.16f;
inline constexpr float kNameMonster[3] = {0.98f, 0.86f, 0.30f};

/// A corpse. Named, targetable, and not worth attacking - the one state where
/// a monster should not be wearing the colour that says "fight me".
inline constexpr float kNameDead[3] = {0.55f, 0.55f, 0.58f};

/// A GM. Darker than the red an aggressive monster gets, which is the
/// distinction the real client draws.
inline constexpr float kNameGm[3] = {0.80f, 0.14f, 0.14f};

/// Matches HudUniforms in hud_shader.h.
struct HudUniforms
{
    float counts[4];
    float atlas[4];
    float boxes[mh::kHudStrings][4];
    float colours[mh::kHudStrings][4];
    float glyphs[mh::kHudStrings * mh::kHudChars][4];
};

/// Matches NameplateUniforms in nameplate_shader.h.
struct NameplateUniforms
{
    float viewProjection[16];
    float counts[4];
    float atlas[4];
    float positions[mh::kNameplateMax][4];
    float colours[mh::kNameplateMax][4];
    float glyphs[mh::kNameplateMax * mh::kNameplateChars][4];
};

/// Matches ChatUniforms in chat_shader.h.
struct ChatUniforms
{
    float placement[4];
    float counts[4];
    float glyphs[mh::kChatLines * mh::kChatColumns][4];
};

/// Matches DialogUniforms in dialog_shader.h.
struct DialogUniforms
{
    float counts[4];
    float atlas[4];
    float panel[4];
    float rects[mh::kDialogRows][4];
    float fills[mh::kDialogRows][4];
    float boxes[mh::kDialogRows][4];
    float colours[mh::kDialogRows][4];
    float glyphs[mh::kDialogRows * mh::kDialogChars][4];
};

/// The death box. Amber for the heading, because that is the colour the game
/// itself uses to say something has happened to you.
inline constexpr float kDialogTitle[3] = {1.00f, 0.84f, 0.48f};
inline constexpr float kDialogText[3] = {0.86f, 0.89f, 0.96f};

/// A button's own label: bright when it can be pressed, and near enough to
/// the panel to disappear into it when it cannot.
inline constexpr float kDialogLabel[3] = {0.98f, 0.98f, 1.00f};
inline constexpr float kDialogLabelOff[3] = {0.42f, 0.44f, 0.50f};

/// And the button behind it. Greyed is not a lighter blue: a disabled button
/// that still looks like a button is one people press and then wonder about,
/// so it loses the colour entirely and keeps only the outline.
inline constexpr float kDialogButton[3] = {0.15f, 0.24f, 0.42f};
inline constexpr float kDialogButtonHot[3] = {0.27f, 0.42f, 0.68f};
inline constexpr float kDialogButtonOff[3] = {0.11f, 0.12f, 0.15f};

/// How far a corpse hauls itself on one press of the jump key, in world units.
///
/// Half a walking pace. A whole one reads as the body getting up and taking a
/// step, which is the opposite of the point; anything much under this is not
/// visible from across a clearing, which is the whole reason for it - the
/// person who might raise you is not standing over you.
inline constexpr float kCorpseDrag = 0.35f;

/// How much of the death clip to replay on that press, counted back from its
/// end. Its tail is the body's last settle onto the ground; its beginning is
/// the character still standing, which is why the clip is rewound to near the
/// end rather than restarted.
inline constexpr int kCorpseTwitchFrames = 3;

/// Where the box put a button last frame, so a click can be tested against
/// what the player is actually looking at.
///
/// Laid out as it is drawn and remembered rather than computed twice: the
/// text is proportional and the box is sized to fit it, so working out where
/// a button is means most of the work of drawing one.
struct DialogButton
{
    float left{};
    float bottom{};
    float width{};
    float height{};
    bool enabled{};
    mh::DeathChoice choice{mh::DeathChoice::None};

    bool holds(float x, float y) const
    {
        return width > 0.0f && x >= left && x < left + width && y >= bottom && y < bottom + height;
    }
};

struct RadarUniforms
{
    float placement[4];
    float mapExtent[4];
    float viewer[4];
    float counts[4];
    float label[mh::kRadarMaxLabel][4];
    float entities[mh::kRadarMaxEntities][4];
};

struct SkyUniforms
{
    float forward[4];
    float right[4];
    float up[4];
    float skyColours[8][4];
    float skyAltitudes[8][4];
    float fogColour[4];
};

const char* backendName(wgpu::BackendType backend)
{
    switch (backend)
    {
    case wgpu::BackendType::D3D12: return "D3D12";
    case wgpu::BackendType::Metal: return "Metal";
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::OpenGL: return "OpenGL";
    case wgpu::BackendType::OpenGLES: return "OpenGLES";
    default: return "other";
    }
}

// Loads the largest MZB in a DAT. Largest because the ferry DATs hold both a
// world and a vessel, and the world is the interesting one.
/// Reads one character out of a DAT: its skeleton, every mesh hung on it, and
/// the textures those meshes name.
///
/// A DAT like ROM/3/6.DAT holds a whole NPC. Player characters are assembled
/// from several files instead - one per equipment slot - which is the same
/// work with more inputs, so this takes a list.
/// A character kept in a form that can still be posed. The geometry is what
/// gets drawn; the skeleton, meshes and animations are what it is rebuilt from
/// every frame.
/// Matches ZoneLineUniforms in zoneline_shader.h.
struct ZoneLineUniforms
{
    float viewProjection[16]{};
    float counts[4]{};
    float lines[mh::kZoneLineMarkers][4]{};
};

struct LoadedCharacter
{
    ffxi::Skeleton skeleton;
    std::vector<ffxi::SkinnedModel> meshes;
    std::map<std::string, ffxi::Animation> animations;
    mh::Character geometry;
};

std::optional<LoadedCharacter> loadCharacter(const std::vector<std::string>& datPaths,
                                             std::unordered_map<std::string, ffxi::Texture>& textures)
{
    LoadedCharacter loaded;
    ffxi::Skeleton& skeleton = loaded.skeleton;
    bool haveSkeleton = false;
    std::vector<ffxi::SkinnedModel>& meshes = loaded.meshes;

    for (const std::string& datPath : datPaths)
    {
        ffxi::DatFile dat{std::filesystem::path{datPath}};

        // The first skeleton found wins. Everything after it has to be hung on
        // the same bones, so a second one would be a different character.
        if (!haveSkeleton)
        {
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
            {
                try
                {
                    skeleton = ffxi::parseSkeleton(chunk);
                    haveSkeleton = true;
                    break;
                }
                catch (const std::exception& e)
                {
                    std::printf("skeleton %.4s: %s\n", chunk.id, e.what());
                }
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
        {
            try
            {
                ffxi::Texture texture = ffxi::parseTexture(chunk);
                textures.insert_or_assign(texture.name, std::move(texture));
            }
            catch (const std::exception&)
            {
            }
        }

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

        // Animations. The race's own file carries most of them; a piece of
        // equipment can bring its own, which is why they are collected from
        // every file rather than only the first.
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkAnimation))
        {
            try
            {
                ffxi::Animation animation = ffxi::parseMo2(chunk);
                loaded.animations.insert_or_assign(animation.name, std::move(animation));
            }
            catch (const std::exception&)
            {
                // Eye and mouth tracks share the chunk type and are not poses.
            }
        }
    }

    if (!haveSkeleton || meshes.empty())
    {
        std::printf("no character in those files\n");
        return std::nullopt;
    }

    loaded.geometry = mh::buildCharacter(mh::bindPose(skeleton), meshes, textures);
    std::printf("character: %zu bones, %zu meshes, %zu triangles, %.2f tall, %zu animations\n",
                skeleton.bones.size(), meshes.size(), loaded.geometry.triangles(), loaded.geometry.height(),
                loaded.animations.size());
    return loaded;
}

/// The interior DATs that belong with a zone file, from assets/subrooms.txt.
///
/// A city zone's own DAT is only its shell - the insides of its buildings are
/// separate files. They are built exactly the same way and their placements are
/// already in zone coordinates, so they need loading and no alignment at all.
///
/// Keyed by the zone's own path relative to the install root, which is what
/// this has to hand; nothing here needs to know a zone id.
std::vector<std::filesystem::path> subroomsFor(const std::filesystem::path& zonePath)
{
    const std::filesystem::path root = ffxi::defaultInstallRoot();
    std::error_code ignored;
    std::string key = std::filesystem::relative(zonePath, root, ignored).generic_string();
    if (key.empty())
    {
        return {};
    }

    // Looked for in every place the renderer gets launched from. It runs as a
    // DLL inside the app, as a standalone exe from the build directory, and
    // from the source tree under tools/play.sh, and only the first of those has
    // a working directory with an assets/ beside it.
    std::vector<std::filesystem::path> candidates;
    if (const char* fromEnv = std::getenv("MOGHOUSE_SUBROOMS"))
    {
        candidates.emplace_back(fromEnv);
    }
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        candidates.push_back(std::filesystem::path{nativeDir} / "assets" / "subrooms.txt");
    }
    if (const char* fontDir = std::getenv("MOGHOUSE_FONT"))
    {
        candidates.push_back(std::filesystem::path{fontDir} / "subrooms.txt");
    }
    candidates.push_back(std::filesystem::path{"assets"} / "subrooms.txt");

    std::ifstream file;
    for (const std::filesystem::path& candidate : candidates)
    {
        file.open(candidate);
        if (file)
        {
            break;
        }
        file.clear();
    }
    if (!file)
    {
        return {};
    }

    std::vector<std::filesystem::path> found;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || line.compare(0, colon, key) != 0)
        {
            continue;
        }
        std::istringstream rest{line.substr(colon + 1)};
        std::string one;
        while (rest >> one)
        {
            found.push_back(root / one);
        }
        break;
    }
    return found;
}

/// The zone's water surfaces, precomputed by tools/ximesh.py.
///
/// Water is a material on each collision triangle - ShallowWater and DeepWater
/// in the server's own TerrainType - rather than a model with a recognisable
/// name or a height on the MZB's cells. Both of those were tried and both were
/// wrong. The server ships the decoded meshes, so the triangles are lifted out
/// of those and written world-space ahead of time; reading them here would mean
/// linking zlib to parse a file that never changes.
size_t loadWater(const std::string& zoneName, mh::Scene& scene)
{
    std::filesystem::path path = std::filesystem::path{"assets"} / "water" / (zoneName + ".water");
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        const std::filesystem::path beside =
            std::filesystem::path{nativeDir} / "assets" / "water" / (zoneName + ".water");
        if (std::filesystem::exists(beside))
        {
            path = beside;
        }
    }
    if (const char* fontDir = std::getenv("MOGHOUSE_FONT"))
    {
        const std::filesystem::path beside = std::filesystem::path{fontDir} / "water" / (zoneName + ".water");
        if (std::filesystem::exists(beside))
        {
            path = beside;
        }
    }

    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return 0;
    }

    char magic[4] = {};
    uint32_t count = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (std::string(magic, 4) != "MHWA" || count == 0 || count > 4000000)
    {
        return 0;
    }

    std::vector<float> corners(static_cast<size_t>(count) * 9);
    file.read(reinterpret_cast<char*>(corners.data()),
              static_cast<std::streamsize>(corners.size() * sizeof(float)));
    if (!file)
    {
        return 0;
    }

    for (uint32_t triangle = 0; triangle < count; ++triangle)
    {
        // Already flat at its pool's waterline - tools/ximesh.py works that
        // out, because the MZB's per-cell height cannot: Windurst Waters
        // writes the same 0.0010 into every water cell, which is a flag rather
        // than a height, and lifting to it drags surfaces under their own bed.
        for (int corner = 0; corner < 3; ++corner)
        {
            const float* p = corners.data() + (static_cast<size_t>(triangle) * 9 + corner * 3);
            mh::Vertex vertex{};
            vertex.position[0] = p[0];
            vertex.position[1] = p[1];
            vertex.position[2] = p[2];
            vertex.normal[1] = 1.0f;
            // World-space UVs, so the surface is continuous across the seams
            // between one collision block and the next.
            vertex.uv[0] = p[0] * 0.06f;
            vertex.uv[1] = p[2] * 0.06f;
            scene.waterIndices.push_back(static_cast<uint32_t>(scene.waterVertices.size()));
            scene.waterVertices.push_back(vertex);
        }
    }
    return count;
}

std::optional<mh::Scene> loadZone(const char* datPath, const char* keyPath, const char* key2Path, std::string& zoneId,
                                     std::unordered_map<std::string, ffxi::Texture>& textures, ffxi::Lighting& lighting,
                                     mh::Collision& collision, std::vector<mh::InteriorLighting>& interiors)
{
    auto keys = ffxi::KeyTable::load(keyPath);
    if (!keys)
    {
        std::printf("could not read a 256-byte key table from %s\n", keyPath);
        return std::nullopt;
    }

    ffxi::DatFile dat{std::filesystem::path{datPath}};

    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkLighting))
    {
        lighting.add(chunk);
    }

    // Textures are not obfuscated, so they need no keys.
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
    {
        try
        {
            ffxi::Texture texture = ffxi::parseTexture(chunk);
            std::string key = texture.name;
            textures.emplace(std::move(key), std::move(texture));
        }
        catch (const std::exception&)
        {
        }
    }

    // Every model in the DAT, keyed by the name a placement refers to it by.
    std::unordered_map<std::string, ffxi::Model> models;
    size_t modelsFailed = 0;
    if (key2Path)
    {
        if (auto keys2 = ffxi::KeyTable::load(key2Path))
        {
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMmb))
            {
                try
                {
                    ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                    std::string key = model.name;
                    models.emplace(std::move(key), std::move(model));
                }
                catch (const std::exception&)
                {
                    ++modelsFailed;
                }
            }
        }
    }

    std::optional<mh::Scene> best;

    // Held because the collision is built from all of them at the end: the
    // zone's shell plus the floors and walls inside each building.
    ffxi::Zone outside;
    std::vector<ffxi::Zone> insides;

    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);

        // Placed models are the visible world; collision geometry is the
        // fallback when the model key table is not available.
        mh::Scene mesh;
        if (!models.empty())
        {
            size_t resolved = 0;
            size_t missing = 0;
            mesh = mh::buildScene(zone, models, textures, resolved, missing);
            if (!mesh.vertices.empty())
            {
                std::printf("  %zu models (%zu unreadable), %zu placements drawn, %zu with no model\n", models.size(),
                            modelsFailed, resolved, missing);
                std::printf("  %zu unique triangles, %zu drawn - instancing saves %.1fx\n", mesh.triangles(),
                            mesh.drawnTriangles(),
                            mesh.triangles() ? static_cast<double>(mesh.drawnTriangles()) / static_cast<double>(mesh.triangles()) : 0.0);
            }
        }
        // Collision geometry is deliberately not drawn. Its meshes carry a
        // flags field with only two values across all 5,921 of them, so it
        // references no material - it is genuine collision, not terrain, and
        // drawing it just covers the world in untextured white.
        if (!best || mesh.vertices.size() > best->vertices.size())
        {
            best = std::move(mesh);
            zoneId = zone.id;
            // The same chunk that produced the visible world produces the
            // ground to stand on, so they cannot disagree.
            outside = std::move(zone);
        }
    }

    // The buildings' insides. Each is a whole little scene of its own - its own
    // models, its own textures, its own placements - so it is loaded the same
    // way the zone was and appended. Windurst Waters is twenty-two of these on
    // top of one zone file, and without them its rooms are empty shells.
    if (best)
    {
        size_t rooms = 0;
        size_t added = 0;
        for (const std::filesystem::path& roomPath : subroomsFor(datPath))
        {
            try
            {
                ffxi::DatFile room{roomPath};
                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkTexture))
                {
                    try
                    {
                        ffxi::Texture texture = ffxi::parseTexture(chunk);
                        std::string key = texture.name;
                        textures.emplace(std::move(key), std::move(texture));
                    }
                    catch (const std::exception&)
                    {
                    }
                }

                std::unordered_map<std::string, ffxi::Model> roomModels;
                if (key2Path)
                {
                    if (auto keys2 = ffxi::KeyTable::load(key2Path))
                    {
                        for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkMmb))
                        {
                            try
                            {
                                ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                                std::string key = model.name;
                                roomModels.emplace(std::move(key), std::move(model));
                            }
                            catch (const std::exception&)
                            {
                            }
                        }
                    }
                }

                // A room's own times of day, kept apart from the zone's rather
                // than merged into them - they describe the inside of one
                // building, not the world.
                ffxi::Lighting inner;
                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkLighting))
                {
                    inner.add(chunk);
                }

                for (const ffxi::Chunk& chunk : room.chunksOfType(ffxi::kChunkMzb))
                {
                    ffxi::Zone inside = ffxi::parseMzb(chunk, *keys);
                    size_t resolved = 0;
                    size_t missing = 0;
                    mh::Scene scene = mh::buildScene(inside, roomModels, textures, resolved, missing);
                    if (!scene.vertices.empty())
                    {
                        // Its floors and walls, which the zone's own collision
                        // does not have: without them a player inside a
                        // building stands on the outdoor ground beneath it and
                        // is stopped by walls that on screen are a doorway.
                        insides.push_back(inside);

                        if (!inner.empty())
                        {
                            // Grown a little, so standing in a doorway does not
                            // flicker between the two sets frame to frame.
                            constexpr float kMargin = 2.0f;
                            interiors.push_back(mh::InteriorLighting{
                                inner,
                                {scene.boundsMin.x - kMargin, scene.boundsMin.y - kMargin, scene.boundsMin.z - kMargin},
                                {scene.boundsMax.x + kMargin, scene.boundsMax.y + kMargin, scene.boundsMax.z + kMargin}});
                        }
                        mh::append(*best, scene);
                        added += resolved;
                        ++rooms;
                    }
                }
            }
            catch (const std::exception& e)
            {
                std::printf("  interior %s did not load: %s\n", roomPath.filename().string().c_str(), e.what());
            }
        }
        std::vector<const ffxi::Zone*> all{&outside};
        for (const ffxi::Zone& one : insides)
        {
            all.push_back(&one);
        }
        collision = mh::Collision{all};

        if (rooms)
        {
            std::printf("  %zu building interiors, %zu placements, %zu with their own lighting\n", rooms, added,
                        interiors.size());
        }
    }
    return best;
}

wgpu::Buffer createBuffer(const wgpu::Device& device, const void* data, size_t size, wgpu::BufferUsage usage)
{
    wgpu::BufferDescriptor descriptor{.usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
    wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
    if (!buffer)
    {
        std::printf("failed to create a %zu byte buffer\n", size);
        return buffer;
    }

    // Written in chunks rather than one call: a single multi-megabyte
    // WriteBuffer stages the whole thing at once, and it is cheap to avoid.
    constexpr size_t kChunk = 4u * 1024u * 1024u;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t written = 0; written < size; written += kChunk)
    {
        const size_t amount = std::min(kChunk, size - written);
        device.GetQueue().WriteBuffer(buffer, written, bytes + written, amount);
    }
    return buffer;
}
/// Writes a 24-bit BMP. Uncompressed and about as simple as an image format
/// gets, which matters because the alternative is pulling in a PNG encoder to
/// look at one frame.
bool writeBmp(const char* path, const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t bytesPerRow)
{
    std::FILE* file = std::fopen(path, "wb");
    if (!file)
    {
        return false;
    }

    const uint32_t rowBytes = width * 3;
    const uint32_t padding = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + padding) * height;
    const uint32_t fileBytes = 54 + imageBytes;

    uint8_t header[54] = {};
    header[0] = 0x42;
    header[1] = 0x4D;
    std::memcpy(header + 2, &fileBytes, 4);
    const uint32_t pixelOffset = 54;
    std::memcpy(header + 10, &pixelOffset, 4);
    const uint32_t infoSize = 40;
    std::memcpy(header + 14, &infoSize, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1;
    const uint16_t bits = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);

    // BMP rows run bottom to top.
    std::vector<uint8_t> row(rowBytes + padding, 0);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* source = bgra + static_cast<size_t>(height - 1 - y) * bytesPerRow;
        for (uint32_t x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = source[x * 4 + 0];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}
} // namespace

void mh::ViewerLink::setEntities(std::vector<RadarEntity> entities)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    entities_ = std::move(entities);
}

void mh::ViewerLink::setZoneLines(std::vector<ZoneLineMarker> lines)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    zoneLines_ = std::move(lines);
}

std::vector<mh::ZoneLineMarker> mh::ViewerLink::zoneLines() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return zoneLines_;
}

std::vector<mh::RadarEntity> mh::ViewerLink::entities() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return entities_;
}

void mh::ViewerLink::pushChat(const std::string& line)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    chat_.push_back(line);
    while (chat_.size() > static_cast<size_t>(mh::kChatLines))
    {
        chat_.pop_front();
    }
}

std::vector<std::string> mh::ViewerLink::chat() const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    return {chat_.begin(), chat_.end()};
}

void mh::ViewerLink::setCharacter(float x, float y, float z, float heading)
{
    const std::lock_guard<std::mutex> guard{mutex_};
    character_[0] = x;
    character_[1] = y;
    character_[2] = z;
    character_[3] = heading;
    haveCharacter_ = true;
}

bool mh::ViewerLink::character(float& x, float& y, float& z, float& heading) const
{
    const std::lock_guard<std::mutex> guard{mutex_};
    if (!haveCharacter_)
    {
        return false;
    }
    x = character_[0];
    y = character_[1];
    z = character_[2];
    heading = character_[3];
    return true;
}

void mh::ViewerLink::submitChat(const std::string& line)
{
    std::lock_guard<std::mutex> lock{mutex_};
    outgoing_.push_back(line);
}

std::optional<std::string> mh::ViewerLink::takeChat()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (outgoing_.empty())
    {
        return std::nullopt;
    }

    std::string line = std::move(outgoing_.front());
    outgoing_.pop_front();
    return line;
}

void mh::ViewerLink::placeCharacter(float x, float y, float z, float heading)
{
    std::lock_guard<std::mutex> lock{mutex_};
    placement_[0] = x;
    placement_[1] = y;
    placement_[2] = z;
    placement_[3] = heading;
    havePlacement_ = true;
}

bool mh::ViewerLink::takePlacement(float& x, float& y, float& z, float& heading)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (!havePlacement_)
    {
        return false;
    }

    x = placement_[0];
    y = placement_[1];
    z = placement_[2];
    heading = placement_[3];
    havePlacement_ = false;
    return true;
}

void mh::ViewerLink::requestJump() { jump_ = true; }

/// Exchange rather than a read and a clear, so a jump is delivered exactly
/// once even if the client polls from a different thread than the one that
/// set it.
bool mh::ViewerLink::takeJump() { return jump_.exchange(false); }

void mh::ViewerLink::requestTalk(uint32_t entityId) { talk_ = entityId; }

/// Exchange rather than a read and a clear, for the same reason takeJump is.
/// Entity 0 is nobody, which is what makes it usable as "nothing pending".
bool mh::ViewerLink::takeTalk(uint32_t& entityId)
{
    entityId = talk_.exchange(0);
    return entityId != 0;
}

void mh::ViewerLink::setDeath(bool dead, bool raiseOffered)
{
    dead_ = dead;
    raiseOffered_ = raiseOffered;
}

bool mh::ViewerLink::dead(bool& raiseOffered) const
{
    raiseOffered = raiseOffered_;
    return dead_;
}

void mh::ViewerLink::setVitals(uint32_t hp, uint32_t mp, uint32_t tp, uint8_t hpPercent, uint8_t mpPercent)
{
    hp_ = hp;
    mp_ = mp;
    tp_ = tp;
    hpPercent_ = hpPercent;
    mpPercent_ = mpPercent;
    vitalsKnown_ = true;
}

void mh::ViewerLink::chooseLink(Link which) { link_ = static_cast<int>(which); }

void mh::ViewerLink::setMusic(std::string path)
{
    std::lock_guard<std::mutex> held{musicLock_};
    if (path != music_)
    {
        music_ = std::move(path);
        musicChanged_ = true;
    }
}

std::string mh::ViewerLink::takeMusic(bool& changed)
{
    std::lock_guard<std::mutex> held{musicLock_};
    changed = musicChanged_;
    musicChanged_ = false;
    return music_;
}

mh::ViewerLink::Link mh::ViewerLink::takeLink()
{
    return static_cast<Link>(link_.exchange(static_cast<int>(Link::None)));
}

mh::ViewerLink::Vitals mh::ViewerLink::vitals() const
{
    return Vitals{hp_.load(), mp_.load(), tp_.load(), hpPercent_.load(), mpPercent_.load(), vitalsKnown_.load()};
}

void mh::ViewerLink::chooseDeath(DeathChoice choice) { deathChoice_ = static_cast<int>(choice); }

/// Exchange rather than a read and a clear, for the same reason takeJump is.
/// A press read twice is two home point requests for one button, and the
/// second arrives at a character the server has already moved.
mh::DeathChoice mh::ViewerLink::takeDeathChoice()
{
    return static_cast<DeathChoice>(deathChoice_.exchange(static_cast<int>(DeathChoice::None)));
}

void mh::ViewerLink::stop() { stop_ = true; }

bool mh::ViewerLink::stopping() const { return stop_; }

int mh::runViewer(const ViewerOptions& options, ViewerLink* link)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string zoneId;

    // How far back the player has asked the camera to sit. Kept apart from
    // camera.distance, which a wall can shorten for a frame - folding the two
    // together means walking through a doorway permanently zooms you in.
    float wantedDistance = 6.0f;

    // Filled the first time entities arrive - see the nameplate loop, which
    // works out the zone from an entity's own id.
    ffxi::EntityNames entityNames;
    bool triedEntityNames = false;
    std::optional<mh::Scene> zone;
    mh::Collision collision;
    std::unordered_map<std::string, ffxi::Texture> textures;
    ffxi::Lighting lighting;

    // Each building interior lights its own inside; see InteriorLighting.
    std::vector<mh::InteriorLighting> interiors;
    if (!options.zonePath.empty())
    {
        const char* keyPath = options.keyTablePath.empty() ? nullptr : options.keyTablePath.c_str();
        if (!keyPath)
        {
            std::printf("set MOGHOUSE_FFXI_KEYTABLE to the 256-byte MZB key table to load a zone\n");
            return 2;
        }
        zone = loadZone(options.zonePath.c_str(), keyPath,
                        options.keyTable2Path.empty() ? nullptr : options.keyTable2Path.c_str(), zoneId, textures,
                        lighting, collision, interiors);
        if (!zone)
        {
            return 1;
        }
        // The character is loaded after the zone so it can share the texture
        // map: a PC in a town wears textures the zone never mentions, and a
        // zone texture the character happens to name should not be read twice.
        if (options.zoneName)
        {
            const size_t water = loadWater(*options.zoneName, *zone);
            if (water)
            {
                std::printf("water: %zu triangles\n", water);
            }
        }
        std::printf("collision: %zu triangles, %zu walls\n", collision.triangleCount(), collision.wallCount());
        std::printf("zone %s: %zu triangles\n", zoneId.c_str(), zone->indices.size() / 3);
        std::printf("  bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", zone->boundsMin.x, zone->boundsMax.x,
                    zone->boundsMin.y, zone->boundsMax.y, zone->boundsMin.z, zone->boundsMax.z);
    }
    else
    {
        std::printf("no DAT given - clearing only. Pass a zone DAT to draw one.\n");
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(mh::kWindowTitle, kWidth, kHeight,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Waiting on a future with a non-zero timeout is opt-in. Without this every
    // WaitAny fails with "Timeout waits are either not enabled or not
    // supported", which reads like a driver limitation and is not one.
    static constexpr wgpu::InstanceFeatureName kInstanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instanceDescriptor{.requiredFeatureCount = std::size(kInstanceFeatures),
                                                .requiredFeatures = kInstanceFeatures};
    wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!instance)
    {
        std::printf("could not create a WebGPU instance\n");
        return 1;
    }

    wgpu::Surface surface = mh::CreateSurface(instance, window);
    if (!surface)
    {
        std::printf("could not create a surface for this window\n");
        return 1;
    }

    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions adapterOptions{.compatibleSurface = surface};
    instance.WaitAny(instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message)
                                             {
                                                 if (status != wgpu::RequestAdapterStatus::Success)
                                                 {
                                                     std::printf("no adapter: %.*s\n", static_cast<int>(message.length), message.data);
                                                     return;
                                                 }
                                                 adapter = std::move(result);
                                             }),
                     UINT64_MAX);
    if (!adapter)
    {
        return 1;
    }

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    std::printf("adapter: %.*s (%s)\n", static_cast<int>(info.device.length), info.device.data, backendName(info.backendType));

    // BC2 is the format FFXI's DXT3 textures already are, but a WebGPU device
    // will not accept it unless the feature is asked for up front.
    const bool supportsBc = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    if (!supportsBc)
    {
        std::printf("this adapter has no BC texture support - compressed textures will be skipped\n");
    }
    const wgpu::FeatureName requiredFeatures[] = {wgpu::FeatureName::TextureCompressionBC};

    wgpu::DeviceDescriptor deviceDescriptor{};
    if (supportsBc)
    {
        deviceDescriptor.requiredFeatureCount = 1;
        deviceDescriptor.requiredFeatures = requiredFeatures;
    }
    deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message)
                                                { std::printf("webgpu error: %.*s\n", static_cast<int>(message.length), message.data); });

    wgpu::Device device;
    instance.WaitAny(adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message)
                                           {
                                               if (status != wgpu::RequestDeviceStatus::Success)
                                               {
                                                   std::printf("no device: %.*s\n", static_cast<int>(message.length), message.data);
                                                   return;
                                               }
                                               device = std::move(result);
                                           }),
                     UINT64_MAX);
    if (!device)
    {
        return 1;
    }

    wgpu::Queue queue = device.GetQueue();

    // The sky is drawn before anything else, at the far plane, with no depth
    // writes - it is a backdrop rather than a surface.
    wgpu::ShaderSourceWGSL skyWgsl;
    skyWgsl.code = mh::kSkyShader;
    wgpu::ShaderModuleDescriptor skyModuleDescriptor{.nextInChain = &skyWgsl};
    wgpu::ShaderModule skyModule = device.CreateShaderModule(&skyModuleDescriptor);

    wgpu::BufferDescriptor skyUniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                .size = sizeof(SkyUniforms)};
    wgpu::Buffer skyUniformBuffer = device.CreateBuffer(&skyUniformDescriptor);

    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);
    const wgpu::TextureFormat surfaceFormat = capabilities.formats[0];

    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);

    int pointWidth = 0;
    int pointHeight = 0;
    SDL_GetWindowSize(window, &pointWidth, &pointHeight);
    std::printf("window: %dx%d points, %dx%d pixels\n", pointWidth, pointHeight, pixelWidth, pixelHeight);

    wgpu::ColorTargetState skyTarget{.format = surfaceFormat};
    wgpu::FragmentState skyFragment{
        .module = skyModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &skyTarget};
    // Depth is compared but never written, so geometry drawn afterwards always
    // wins and the sky fills only what is left.
    wgpu::DepthStencilState skyDepth{.format = kDepthFormat,
                                     .depthWriteEnabled = wgpu::OptionalBool::False,
                                     .depthCompare = wgpu::CompareFunction::Always};
    wgpu::RenderPipelineDescriptor skyPipelineDescriptor{
        .vertex = {.module = skyModule, .entryPoint = "vertexMain"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &skyDepth,
        .fragment = &skyFragment};
    wgpu::RenderPipeline skyPipeline = device.CreateRenderPipeline(&skyPipelineDescriptor);

    wgpu::BindGroupEntry skyEntry{.binding = 0, .buffer = skyUniformBuffer, .size = sizeof(SkyUniforms)};
    wgpu::BindGroupDescriptor skyBindGroupDescriptor{
        .layout = skyPipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &skyEntry};
    wgpu::BindGroup skyBindGroup = device.CreateBindGroup(&skyBindGroupDescriptor);

    wgpu::Texture depthTexture;
    auto configure = [&](uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration configuration{.device = device,
                                                 .format = surfaceFormat,
                                                 // CopySrc so a frame can be read back; see writeBmp.
                                                 .usage = wgpu::TextureUsage::RenderAttachment |
                                                          wgpu::TextureUsage::CopySrc,
                                                 .width = width,
                                                 .height = height,
                                                 .presentMode = wgpu::PresentMode::Fifo};
        surface.Configure(&configuration);

        wgpu::TextureDescriptor depthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                .dimension = wgpu::TextureDimension::e2D,
                                                .size = {width, height, 1},
                                                .format = kDepthFormat,
                                                .mipLevelCount = 1,
                                                .sampleCount = 1};
        depthTexture = device.CreateTexture(&depthDescriptor);
    };
    configure(static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));

    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    wgpu::Buffer instanceBuffer;
    wgpu::Buffer waterVertexBuffer;
    wgpu::Buffer waterIndexBuffer;
    wgpu::RenderPipeline waterPipeline;
    wgpu::BindGroup waterBindGroup;
    uint32_t waterIndexCount = 0;
    wgpu::Buffer uniformBuffer;
    wgpu::RenderPipeline pipeline;
    wgpu::RenderPipeline cutoutPipeline;
    wgpu::RenderPipeline translucentPipeline;
    wgpu::BindGroupLayout zoneBindGroupLayout;
    wgpu::Sampler sampler;
    wgpu::Texture whiteTexture;
    // Bound to water meshes, which name no texture at all in their mesh
    // header - FFXI supplies theirs some other way, and until that is worked
    // out the white fallback makes a canal look like a sheet of paper.
    //
    // A placeholder, and deliberately a recognisable one: this is a flat
    // colour, not water, and it should look like a stand-in rather than like
    // a rendering someone signed off.
    wgpu::Texture waterFallbackTexture;
    std::vector<wgpu::Texture> batchTextures;
    std::vector<wgpu::BindGroup> batchBindGroups;
    uint32_t indexCount = 0;

    if (zone && !zone->indices.empty())
    {
        vertexBuffer = createBuffer(device, zone->vertices.data(), zone->vertices.size() * sizeof(mh::Vertex),
                                    wgpu::BufferUsage::Vertex);
        indexBuffer = createBuffer(device, zone->indices.data(), zone->indices.size() * sizeof(uint32_t),
                                   wgpu::BufferUsage::Index);
        instanceBuffer = createBuffer(device, zone->instances.data(), zone->instances.size() * sizeof(float),
                                      wgpu::BufferUsage::Vertex);
        indexCount = static_cast<uint32_t>(zone->indices.size());
        std::printf("buffers created\n");

        wgpu::BufferDescriptor uniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                 .size = sizeof(Uniforms)};
        uniformBuffer = device.CreateBuffer(&uniformDescriptor);

        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = mh::kZoneShader;
        wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl};
        wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

        // Location 7, not 3: the instance matrix already holds 3 through 6, and
        // the two buffers share one location space.
        wgpu::VertexAttribute attributes[4] = {
            {.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
            {.format = wgpu::VertexFormat::Float32x3, .offset = 3 * sizeof(float), .shaderLocation = 1},
            {.format = wgpu::VertexFormat::Float32x2, .offset = 6 * sizeof(float), .shaderLocation = 2},
            {.format = wgpu::VertexFormat::Unorm8x4, .offset = 8 * sizeof(float), .shaderLocation = 7}};
        wgpu::VertexAttribute instanceAttributes[4] = {
            {.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 3},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 4 * sizeof(float), .shaderLocation = 4},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 8 * sizeof(float), .shaderLocation = 5},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 12 * sizeof(float), .shaderLocation = 6}};

        wgpu::VertexBufferLayout vertexLayout{.stepMode = wgpu::VertexStepMode::Vertex,
                                              .arrayStride = sizeof(mh::Vertex),
                                              .attributeCount = 4,
                                              .attributes = attributes};
        // Stepping per instance rather than per vertex is the whole trick: one
        // copy of the geometry, one matrix per placement.
        wgpu::VertexBufferLayout instanceLayout{.stepMode = wgpu::VertexStepMode::Instance,
                                                .arrayStride = 16 * sizeof(float),
                                                .attributeCount = 4,
                                                .attributes = instanceAttributes};
        wgpu::VertexBufferLayout bufferLayouts[2] = {vertexLayout, instanceLayout};

        // An explicit layout shared by both pipelines. Letting each derive its
        // own default layout makes bind groups built for one incompatible with
        // the other, which fails at draw time rather than at creation.
        wgpu::BindGroupLayoutEntry layoutEntries[3] = {};
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        layoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        layoutEntries[1].binding = 1;
        layoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        layoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        layoutEntries[2].binding = 2;
        layoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{.entryCount = 3, .entries = layoutEntries};
        zoneBindGroupLayout = device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                .bindGroupLayouts = &zoneBindGroupLayout};
        wgpu::PipelineLayout sharedLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

        wgpu::ColorTargetState colorTarget{.format = surfaceFormat};
        wgpu::FragmentState fragment{.module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &colorTarget};
        wgpu::DepthStencilState depthStencil{.format = kDepthFormat,
                                             .depthWriteEnabled = wgpu::OptionalBool::True,
                                             .depthCompare = wgpu::CompareFunction::Less};

        wgpu::RenderPipelineDescriptor pipelineDescriptor{
            .layout = sharedLayout,
            .vertex = {.module = module, .entryPoint = "vertexMain", .bufferCount = 2, .buffers = bufferLayouts},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &depthStencil,
            .fragment = &fragment};
        pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

        // Same pipeline, alpha-cutout fragment shader. Which one a batch uses
        // comes from its mesh header rather than from one global choice.
        wgpu::FragmentState cutoutFragment{
            .module = module, .entryPoint = "fragmentCutout", .targetCount = 1, .targets = &colorTarget};
        wgpu::RenderPipelineDescriptor cutoutDescriptor = pipelineDescriptor;
        cutoutDescriptor.fragment = &cutoutFragment;
        cutoutPipeline = device.CreateRenderPipeline(&cutoutDescriptor);

        // And once more for water: the same shader, blended, and not writing
        // depth so a surface does not hide the one behind it. Bastok Markets
        // has two water meshes stacked - a darker body with a lighter sheet
        // over it - and with depth writes on, whichever drew first won.
        wgpu::BlendState surfaceBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState surfaceTarget{.format = surfaceFormat, .blend = &surfaceBlend};
        wgpu::FragmentState surfaceFragment{
            .module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &surfaceTarget};
        wgpu::DepthStencilState surfaceDepth{.format = kDepthFormat,
                                             .depthWriteEnabled = wgpu::OptionalBool::False,
                                             .depthCompare = wgpu::CompareFunction::Less};
        wgpu::RenderPipelineDescriptor surfaceDescriptor = pipelineDescriptor;
        surfaceDescriptor.fragment = &surfaceFragment;
        surfaceDescriptor.depthStencil = &surfaceDepth;
        translucentPipeline = device.CreateRenderPipeline(&surfaceDescriptor);

        // WebGPU has no bindless arrays, so each texture needs its own bind
        // group and its own draw. Fine at a zone's few dozen textures; this is
        // the thing that will need atlasing or caching at a larger scale.
        wgpu::SamplerDescriptor samplerDescriptor{};
        samplerDescriptor.addressModeU = wgpu::AddressMode::Repeat;
        samplerDescriptor.addressModeV = wgpu::AddressMode::Repeat;
        samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
        samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
        sampler = device.CreateSampler(&samplerDescriptor);

        whiteTexture = mh::createWhiteTexture(device);
        // Dark. Bastok's water is nearly black at night and a deep slate by
        // day, and a cheerful mid-blue placeholder reads as a mistake in a
        // city built out of grey stone.
        waterFallbackTexture = mh::createSolidTexture(device, 26, 46, 54, 190);
        const wgpu::TextureView whiteView = whiteTexture.CreateView();
        const wgpu::TextureView waterFallbackView = waterFallbackTexture.CreateView();

        if (!zone->waterIndices.empty())
        {
            waterVertexBuffer = createBuffer(device, zone->waterVertices.data(),
                                             zone->waterVertices.size() * sizeof(mh::Vertex), wgpu::BufferUsage::Vertex);
            waterIndexBuffer = createBuffer(device, zone->waterIndices.data(),
                                            zone->waterIndices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);
            waterIndexCount = static_cast<uint32_t>(zone->waterIndices.size());

            wgpu::ShaderSourceWGSL waterWgsl;
            waterWgsl.code = mh::kWaterShader;
            wgpu::ShaderModuleDescriptor waterModuleDescriptor{.nextInChain = &waterWgsl};
            wgpu::ShaderModule waterModule = device.CreateShaderModule(&waterModuleDescriptor);

            // Blended, and writing no depth: water is a surface you see through,
            // and letting it write depth hides whatever is under it.
            wgpu::BlendState waterBlend{};
            waterBlend.color.operation = wgpu::BlendOperation::Add;
            waterBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
            waterBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
            waterBlend.alpha.operation = wgpu::BlendOperation::Add;
            waterBlend.alpha.srcFactor = wgpu::BlendFactor::One;
            waterBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;

            wgpu::ColorTargetState waterTarget{.format = surfaceFormat, .blend = &waterBlend};
            wgpu::FragmentState waterFragment{
                .module = waterModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &waterTarget};
            wgpu::DepthStencilState waterDepth{.format = kDepthFormat,
                                               .depthWriteEnabled = wgpu::OptionalBool::False,
                                               .depthCompare = wgpu::CompareFunction::Less};

            // FFXI's own water texture, scrolled, rather than an invented
            // colour. The models nothing places - kw01 for rivers, ike for
            // ponds, umi1 for sea - all carry one of these.
            // Blue, not white. This looks for a texture by name and falls
            // back when it finds none, and it finds none in Bastok Markets -
            // so every pooled quad painted an opaque white patch over the
            // floor it was sitting on. Read as missing floor, which is fair.
            wgpu::TextureView waterView = waterFallbackTexture.CreateView();
            for (const char* candidate : {"effect  kaw1", "effect  ike1", "effect  ike2", "effect  umna", "effect  nami"})
            {
                auto found = textures.find(candidate);
                if (found != textures.end())
                {
                    wgpu::Texture gpu = mh::uploadTexture(device, found->second);
                    if (gpu)
                    {
                        batchTextures.push_back(gpu);
                        waterView = batchTextures.back().CreateView();
                        std::printf("water texture: %s\n", candidate);
                        break;
                    }
                }
            }

            wgpu::BindGroupLayoutEntry waterLayoutEntries[3] = {};
            waterLayoutEntries[0].binding = 0;
            waterLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
            waterLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
            waterLayoutEntries[1].binding = 1;
            waterLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
            waterLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
            waterLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            waterLayoutEntries[2].binding = 2;
            waterLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
            waterLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
            wgpu::BindGroupLayoutDescriptor waterBglDescriptor{.entryCount = 3, .entries = waterLayoutEntries};
            wgpu::BindGroupLayout waterBgl = device.CreateBindGroupLayout(&waterBglDescriptor);
            wgpu::PipelineLayoutDescriptor waterPlDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &waterBgl};
            wgpu::PipelineLayout waterPl = device.CreatePipelineLayout(&waterPlDescriptor);

            wgpu::RenderPipelineDescriptor waterPipelineDescriptor{
                .layout = waterPl,
                .vertex = {.module = waterModule, .entryPoint = "vertexMain", .bufferCount = 1, .buffers = &vertexLayout},
                .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
                .depthStencil = &waterDepth,
                .fragment = &waterFragment};
            waterPipeline = device.CreateRenderPipeline(&waterPipelineDescriptor);

            wgpu::BindGroupEntry waterEntries[3] = {};
            waterEntries[0].binding = 0;
            waterEntries[0].buffer = uniformBuffer;
            waterEntries[0].size = sizeof(Uniforms);
            waterEntries[1].binding = 1;
            waterEntries[1].textureView = waterView;
            waterEntries[2].binding = 2;
            waterEntries[2].sampler = sampler;
            wgpu::BindGroupDescriptor waterBgDescriptor{.layout = waterBgl, .entryCount = 3, .entries = waterEntries};
            waterBindGroup = device.CreateBindGroup(&waterBgDescriptor);

            std::printf("water: %zu quads\n", zone->waterIndices.size() / 6);
        }

        std::unordered_map<std::string, wgpu::TextureView> uploadedViews;
        size_t uploaded = 0;
        size_t untextured = 0;
        // Counted apart from untextured. A mesh naming a texture the DAT does
        // not hold and a mesh naming none at all both land on the white
        // fallback and look identical on screen, but only the first is a
        // lookup failure - and only the first was being counted. Bastok
        // Markets' water meshes are the second kind, so the line said "0 with
        // no texture" while the water rendered as a sheet of pure white.
        size_t unnamed = 0;
        for (const mh::InstancedDraw& batch : zone->draws)
        {
            if (batch.texture.empty())
            {
                ++unnamed;
            }
            // Cached by name: instancing produces one draw per mesh, and many
            // meshes share a texture. Uploading per draw meant 397 GPU textures
            // for 46 distinct images.
            wgpu::TextureView view = batch.water ? waterFallbackView : whiteView;
            if (!batch.texture.empty())
            {
                auto cached = uploadedViews.find(batch.texture);
                if (cached != uploadedViews.end())
                {
                    view = cached->second;
                }
                else
                {
                    auto found = textures.find(batch.texture);
                    if (found != textures.end())
                    {
                        wgpu::Texture gpu = mh::uploadTexture(device, found->second);
                        if (gpu)
                        {
                            batchTextures.push_back(gpu);
                            view = batchTextures.back().CreateView();
                            uploadedViews.emplace(batch.texture, view);
                            ++uploaded;
                        }
                    }
                    else
                    {
                        ++untextured;
                    }
                }
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            batchBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }
        if (unnamed)
        {
            std::printf("  %zu draws name no texture at all - they render white\n", unnamed);
        }
        std::printf("%zu draws, %zu textures uploaded, %zu with no texture in this DAT\n", zone->draws.size(),
                    uploaded, untextured);
    }

    // --- the map ------------------------------------------------------------
    // Baked once, straight down, through the pipeline that draws the zone - so
    // the radar shows the world as it actually looks rather than a schematic
    // redrawn from the same data.
    wgpu::Texture mapTexture;
    if (zone && pipeline && !zone->draws.empty())
    {
        constexpr uint32_t kMapSize = 2048;

        wgpu::TextureDescriptor mapDescriptor{
            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {kMapSize, kMapSize, 1},
            .format = surfaceFormat,
            .mipLevelCount = 1,
            .sampleCount = 1};
        mapTexture = device.CreateTexture(&mapDescriptor);

        wgpu::TextureDescriptor mapDepthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                   .dimension = wgpu::TextureDimension::e2D,
                                                   .size = {kMapSize, kMapSize, 1},
                                                   .format = kDepthFormat,
                                                   .mipLevelCount = 1,
                                                   .sampleCount = 1};
        wgpu::Texture mapDepth = device.CreateTexture(&mapDepthDescriptor);

        // A square covering the zone, so the map keeps the world aspect and a
        // step north is the same number of pixels as a step east.
        const mh::Vec3 lo = zone->boundsMin;
        const mh::Vec3 hi = zone->boundsMax;
        const float half = std::max(hi.x - lo.x, hi.z - lo.z) * 0.5f;
        const mh::Vec3 middle = zone->centre();

        Uniforms mapUniforms{};
        // Straight down, with +z up the screen and +x to the right.
        //
        // The left and right of the projection are swapped on purpose, and it
        // is not about the world's frame - it is about this camera. Looking
        // down with up = +z makes cross(forward, up) = -x, so a plain
        // projection puts +x on the *left* of the image. rasteriseWalkable
        // numbers its columns from minimum x upward, and the radar's dots are
        // placed the same way, so the bake has to match them rather than the
        // other way round.
        //
        // Removing this swap costs 47 points on the alignment score: 52.6% of
        // walkable area with terrain drawn on it against 99.8% with it.
        const mh::Mat4 mapView = mh::lookAt({middle.x, hi.y + 100.0f, middle.z}, middle, {0.0f, 0.0f, 1.0f});
        const mh::Mat4 mapProjection = mh::orthographic(half, -half, -half, half, 1.0f, (hi.y - lo.y) + 400.0f);
        const mh::Mat4 mapViewProjection = mapProjection * mapView;
        std::memcpy(mapUniforms.viewProjection, mapViewProjection.m, sizeof(mapUniforms.viewProjection));

        // Flat and unfogged. A map lit from an angle reads as terrain in
        // shadow rather than as ground, and fog would erase the far half.
        const mh::Vec3 down = mh::normalise(mh::Vec3{0.0f, 1.0f, 0.0f});
        mapUniforms.lightDirection[0] = down.x;
        mapUniforms.lightDirection[1] = down.y;
        mapUniforms.lightDirection[2] = down.z;
        mapUniforms.ambient[0] = mapUniforms.ambient[1] = mapUniforms.ambient[2] = 0.75f;
        mapUniforms.sunlight[0] = mapUniforms.sunlight[1] = mapUniforms.sunlight[2] = 0.35f;
        mapUniforms.fogRange[0] = 1e9f;
        mapUniforms.fogRange[1] = 1e9f;
        queue.WriteBuffer(uniformBuffer, 0, &mapUniforms, sizeof(mapUniforms));

        wgpu::RenderPassColorAttachment mapColour{.view = mapTexture.CreateView(),
                                                  .loadOp = wgpu::LoadOp::Clear,
                                                  .storeOp = wgpu::StoreOp::Store,
                                                  .clearValue = {0.0f, 0.0f, 0.0f, 0.0f}};
        wgpu::RenderPassDepthStencilAttachment mapDepthAttachment{.view = mapDepth.CreateView(),
                                                                  .depthLoadOp = wgpu::LoadOp::Clear,
                                                                  .depthStoreOp = wgpu::StoreOp::Store,
                                                                  .depthClearValue = 1.0f};
        wgpu::RenderPassDescriptor mapPassDescriptor{.colorAttachmentCount = 1,
                                                     .colorAttachments = &mapColour,
                                                     .depthStencilAttachment = &mapDepthAttachment};

        wgpu::CommandEncoder mapEncoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder mapPass = mapEncoder.BeginRenderPass(&mapPassDescriptor);
        mapPass.SetVertexBuffer(0, vertexBuffer);
        mapPass.SetVertexBuffer(1, instanceBuffer);
        mapPass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
        for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
        {
            const mh::InstancedDraw& draw = zone->draws[i];
            mapPass.SetPipeline(draw.cutout ? cutoutPipeline : pipeline);
            mapPass.SetBindGroup(0, batchBindGroups[i]);
            mapPass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
        }
        if (waterIndexCount && waterPipeline)
        {
            mapPass.SetPipeline(waterPipeline);
            mapPass.SetBindGroup(0, waterBindGroup);
            mapPass.SetVertexBuffer(0, waterVertexBuffer);
            mapPass.SetIndexBuffer(waterIndexBuffer, wgpu::IndexFormat::Uint32);
            mapPass.DrawIndexed(waterIndexCount);
        }
        mapPass.End();
        wgpu::CommandBuffer mapCommands = mapEncoder.Finish();
        queue.Submit(1, &mapCommands);

        std::printf("map: baked %ux%u covering %.0f units, centred on %.0f %.0f\n", kMapSize, kMapSize, half * 2.0f,
                    middle.x, middle.z);

        // mapPath writes the bake out so it can be looked at directly.
        if (options.mapPath)
        {
            const uint32_t bytesPerRow = (kMapSize * 4 + 255) / 256 * 256;
            wgpu::BufferDescriptor readDescriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                  .size = static_cast<uint64_t>(bytesPerRow) * kMapSize};
            wgpu::Buffer readback = device.CreateBuffer(&readDescriptor);

            wgpu::CommandEncoder copyEncoder = device.CreateCommandEncoder();
            wgpu::TexelCopyTextureInfo source{.texture = mapTexture};
            wgpu::TexelCopyBufferInfo destination{
                .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = kMapSize}, .buffer = readback};
            const wgpu::Extent3D extent{kMapSize, kMapSize, 1};
            copyEncoder.CopyTextureToBuffer(&source, &destination, &extent);
            wgpu::CommandBuffer copyCommands = copyEncoder.Finish();
            queue.Submit(1, &copyCommands);

            // The mask over the same square, so the two can be checked against
            // each other. They are only useful together, and an offset here
            // would put every radar dot the same distance off the terrain it
            // is meant to sit on.
            {
                const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMapSize, middle, half);
                const std::string maskPath = *options.mapPath + ".mask.pgm";
                if (std::FILE* file = std::fopen(maskPath.c_str(), "wb"))
                {
                    std::fprintf(file, "P5\n%u %u\n255\n", kMapSize, kMapSize);
                    std::fwrite(mask.data(), 1, mask.size(), file);
                    std::fclose(file);
                    std::printf("wrote %s\n", maskPath.c_str());
                }
            }

            bool mapped = false;
            wgpu::Future future = readback.MapAsync(wgpu::MapMode::Read, 0, wgpu::kWholeMapSize,
                                                    wgpu::CallbackMode::AllowProcessEvents,
                                                    [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                                                    { mapped = status == wgpu::MapAsyncStatus::Success; });
            instance.WaitAny(future, UINT64_MAX);
            if (mapped)
            {
                const auto* pixels = static_cast<const uint8_t*>(readback.GetConstMappedRange());
                if (pixels && writeBmp(options.mapPath->c_str(), pixels, kMapSize, kMapSize, bytesPerRow))
                {
                    std::printf("wrote %s\n", options.mapPath->c_str());
                }
                readback.Unmap();
            }
        }
    }

    // --- the radar -----------------------------------------------------------
    wgpu::Texture maskTexture;
    wgpu::Buffer radarUniformBuffer;
    wgpu::RenderPipeline radarPipeline;
    wgpu::BindGroup radarBindGroup;

    // The glyph atlas. Text is worth doing without rather than failing to
    // open a window over, so an absent atlas leaves the nameplates off and
    // everything else running.
    //
    // The atlas is copied beside the renderer executable, but the renderer
    // also runs in-process from the console, where "the executable" is a .NET
    // host somewhere else entirely, and run.ps1 starts from the repo root
    // where there is no assets folder at all. So try the places it can be,
    // and say which were tried if it is in none of them - an absent atlas
    // takes the clock, the compass, the coordinates and the whole chat panel
    // with it, and did so in silence.
    std::vector<std::filesystem::path> fontCandidates;
    if (const char* installedAt = SDL_GetBasePath())
    {
        fontCandidates.emplace_back(std::filesystem::path{installedAt} / "assets");
    }
    if (const char* nativeDir = std::getenv("MOGHOUSE_NATIVE_DIR"))
    {
        fontCandidates.emplace_back(std::filesystem::path{nativeDir} / "assets");
    }
    const std::filesystem::path here = std::filesystem::current_path();
    fontCandidates.push_back(here / "assets");
    fontCandidates.push_back(here / "build-renderer" / "assets");
    fontCandidates.push_back(here / "renderer" / "assets");

    mh::TextFont textFont;
    for (const std::filesystem::path& candidate : fontCandidates)
    {
        textFont = mh::loadTextFont(candidate.string());
        if (!textFont.empty())
        {
            break;
        }
    }
    if (textFont.empty())
    {
        std::printf("no font atlas found - the HUD will not draw. Set MOGHOUSE_FONT. Looked in:\n");
        for (const std::filesystem::path& candidate : fontCandidates)
        {
            std::printf("  %s\n", candidate.string().c_str());
        }
    }

    wgpu::Texture fontTexture;
    if (!textFont.empty())
    {
        wgpu::TextureDescriptor fontDescriptor{};
        fontDescriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        fontDescriptor.dimension = wgpu::TextureDimension::e2D;
        fontDescriptor.size = {textFont.width, textFont.height, 1};
        fontDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
        fontDescriptor.mipLevelCount = 1;
        fontDescriptor.sampleCount = 1;
        fontTexture = device.CreateTexture(&fontDescriptor);

        wgpu::TexelCopyTextureInfo destination{};
        destination.texture = fontTexture;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = textFont.width * 4;
        layout.rowsPerImage = textFont.height;
        const wgpu::Extent3D extent{textFont.width, textFont.height, 1};
        queue.WriteTexture(&destination, textFont.pixels.data(), textFont.pixels.size(), &layout, &extent);
    }

    // Shared by the nameplates and the HUD, which draw the same atlas.
    wgpu::Sampler fontSampler;
    if (fontTexture)
    {
        wgpu::SamplerDescriptor fontSamplerDescriptor{};
        fontSamplerDescriptor.magFilter = wgpu::FilterMode::Linear;
        fontSamplerDescriptor.minFilter = wgpu::FilterMode::Linear;
        fontSamplerDescriptor.addressModeU = wgpu::AddressMode::ClampToEdge;
        fontSamplerDescriptor.addressModeV = wgpu::AddressMode::ClampToEdge;
        fontSampler = device.CreateSampler(&fontSamplerDescriptor);
    }

    wgpu::Buffer hudUniformBuffer;
    wgpu::RenderPipeline hudPipeline;
    wgpu::BindGroup hudBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor hudBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                   .size = sizeof(HudUniforms)};
        hudUniformBuffer = device.CreateBuffer(&hudBufferDescriptor);

        wgpu::ShaderSourceWGSL hudWgsl;
        hudWgsl.code = mh::kHudShader;
        wgpu::ShaderModuleDescriptor hudModuleDescriptor{.nextInChain = &hudWgsl};
        wgpu::ShaderModule hudModule = device.CreateShaderModule(&hudModuleDescriptor);

        wgpu::BindGroupLayoutEntry hudLayoutEntries[3] = {};
        hudLayoutEntries[0].binding = 0;
        hudLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        hudLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        hudLayoutEntries[1].binding = 1;
        hudLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        hudLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        hudLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        hudLayoutEntries[2].binding = 2;
        hudLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        hudLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor hudLayoutDescriptor{.entryCount = 3, .entries = hudLayoutEntries};
        wgpu::BindGroupLayout hudBindGroupLayout = device.CreateBindGroupLayout(&hudLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor hudPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                   .bindGroupLayouts = &hudBindGroupLayout};
        wgpu::PipelineLayout hudPipelineLayout = device.CreatePipelineLayout(&hudPipelineLayoutDescriptor);

        wgpu::BlendState hudBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState hudTarget{.format = surfaceFormat, .blend = &hudBlend};
        wgpu::FragmentState hudFragment{
            .module = hudModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &hudTarget};
        wgpu::DepthStencilState hudDepth{.format = kDepthFormat,
                                         .depthWriteEnabled = wgpu::OptionalBool::False,
                                         .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor hudPipelineDescriptor{
            .layout = hudPipelineLayout,
            .vertex = {.module = hudModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &hudDepth,
            .fragment = &hudFragment};
        hudPipeline = device.CreateRenderPipeline(&hudPipelineDescriptor);

        wgpu::BindGroupEntry hudEntries[3] = {};
        hudEntries[0].binding = 0;
        hudEntries[0].buffer = hudUniformBuffer;
        hudEntries[0].size = sizeof(HudUniforms);
        hudEntries[1].binding = 1;
        hudEntries[1].textureView = fontTexture.CreateView();
        hudEntries[2].binding = 2;
        hudEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor hudBindGroupDescriptor{
            .layout = hudBindGroupLayout, .entryCount = 3, .entries = hudEntries};
        hudBindGroup = device.CreateBindGroup(&hudBindGroupDescriptor);
    }

    wgpu::Buffer plateUniformBuffer;
    wgpu::RenderPipeline platePipeline;
    wgpu::BindGroup plateBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor plateBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                     .size = sizeof(NameplateUniforms)};
        plateUniformBuffer = device.CreateBuffer(&plateBufferDescriptor);

        wgpu::ShaderSourceWGSL plateWgsl;
        plateWgsl.code = mh::kNameplateShader;
        wgpu::ShaderModuleDescriptor plateModuleDescriptor{.nextInChain = &plateWgsl};
        wgpu::ShaderModule plateModule = device.CreateShaderModule(&plateModuleDescriptor);

        wgpu::BindGroupLayoutEntry plateLayoutEntries[3] = {};
        plateLayoutEntries[0].binding = 0;
        plateLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        plateLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        plateLayoutEntries[1].binding = 1;
        plateLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        plateLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        plateLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        plateLayoutEntries[2].binding = 2;
        plateLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        plateLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor plateLayoutDescriptor{.entryCount = 3, .entries = plateLayoutEntries};
        wgpu::BindGroupLayout plateBindGroupLayout = device.CreateBindGroupLayout(&plateLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor platePipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                     .bindGroupLayouts = &plateBindGroupLayout};
        wgpu::PipelineLayout platePipelineLayout = device.CreatePipelineLayout(&platePipelineLayoutDescriptor);

        wgpu::BlendState plateBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState plateTarget{.format = surfaceFormat, .blend = &plateBlend};
        wgpu::FragmentState plateFragment{
            .module = plateModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &plateTarget};

        // Always, because the depth test cannot help here: the whole pass is
        // one fullscreen triangle at depth zero and each label is projected in
        // the fragment shader, so there is no per-label depth to compare.
        // Whether a name is behind a wall is decided on the CPU, with the same
        // raycast the camera uses - see where the plates are laid out.
        wgpu::DepthStencilState plateDepth{.format = kDepthFormat,
                                           .depthWriteEnabled = false,
                                           .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor platePipelineDescriptor{
            .layout = platePipelineLayout,
            .vertex = {.module = plateModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &plateDepth,
            .fragment = &plateFragment};
        platePipeline = device.CreateRenderPipeline(&platePipelineDescriptor);

        wgpu::BindGroupEntry plateEntries[3] = {};
        plateEntries[0].binding = 0;
        plateEntries[0].buffer = plateUniformBuffer;
        plateEntries[0].size = sizeof(NameplateUniforms);
        plateEntries[1].binding = 1;
        plateEntries[1].textureView = fontTexture.CreateView();
        plateEntries[2].binding = 2;
        plateEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor plateBindGroupDescriptor{
            .layout = plateBindGroupLayout, .entryCount = 3, .entries = plateEntries};
        plateBindGroup = device.CreateBindGroup(&plateBindGroupDescriptor);
    }

    wgpu::Buffer zoneLineUniformBuffer;
    wgpu::RenderPipeline zoneLinePipeline;
    wgpu::BindGroup zoneLineBindGroup;
    {
        wgpu::BufferDescriptor zoneLineBufferDescriptor{
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(ZoneLineUniforms)};
        zoneLineUniformBuffer = device.CreateBuffer(&zoneLineBufferDescriptor);

        wgpu::ShaderSourceWGSL zoneLineWgsl;
        zoneLineWgsl.code = mh::kZoneLineShader;
        wgpu::ShaderModuleDescriptor zoneLineModuleDescriptor{.nextInChain = &zoneLineWgsl};
        wgpu::ShaderModule zoneLineModule = device.CreateShaderModule(&zoneLineModuleDescriptor);

        wgpu::BindGroupLayoutEntry zoneLineLayoutEntry{};
        zoneLineLayoutEntry.binding = 0;
        zoneLineLayoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        zoneLineLayoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;

        wgpu::BindGroupLayoutDescriptor zoneLineLayoutDescriptor{.entryCount = 1, .entries = &zoneLineLayoutEntry};
        wgpu::BindGroupLayout zoneLineBindGroupLayout = device.CreateBindGroupLayout(&zoneLineLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor zoneLinePipelineLayoutDescriptor{
            .bindGroupLayoutCount = 1, .bindGroupLayouts = &zoneLineBindGroupLayout};
        wgpu::PipelineLayout zoneLinePipelineLayout = device.CreatePipelineLayout(&zoneLinePipelineLayoutDescriptor);

        // Added rather than blended over: a glow brightens what is behind it.
        wgpu::BlendState zoneLineBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::One},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::One}};
        wgpu::ColorTargetState zoneLineTarget{.format = surfaceFormat, .blend = &zoneLineBlend};
        wgpu::FragmentState zoneLineFragment{.module = zoneLineModule,
                                             .entryPoint = "fragmentMain",
                                             .targetCount = 1,
                                             .targets = &zoneLineTarget};

        // Occluded by the world but not writing depth: a line behind a wall
        // stays behind it, and two rings overlapping do not cut each other up.
        wgpu::DepthStencilState zoneLineDepth{.format = kDepthFormat,
                                              .depthWriteEnabled = false,
                                              .depthCompare = wgpu::CompareFunction::LessEqual};

        wgpu::RenderPipelineDescriptor zoneLinePipelineDescriptor{
            .layout = zoneLinePipelineLayout,
            .vertex = {.module = zoneLineModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &zoneLineDepth,
            .fragment = &zoneLineFragment};
        zoneLinePipeline = device.CreateRenderPipeline(&zoneLinePipelineDescriptor);

        wgpu::BindGroupEntry zoneLineEntry{};
        zoneLineEntry.binding = 0;
        zoneLineEntry.buffer = zoneLineUniformBuffer;
        zoneLineEntry.size = sizeof(ZoneLineUniforms);
        wgpu::BindGroupDescriptor zoneLineBindGroupDescriptor{
            .layout = zoneLineBindGroupLayout, .entryCount = 1, .entries = &zoneLineEntry};
        zoneLineBindGroup = device.CreateBindGroup(&zoneLineBindGroupDescriptor);
    }

    wgpu::Buffer chatUniformBuffer;
    wgpu::RenderPipeline chatPipeline;
    wgpu::BindGroup chatBindGroup;
    {
        wgpu::BufferDescriptor chatBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                    .size = sizeof(ChatUniforms)};
        chatUniformBuffer = device.CreateBuffer(&chatBufferDescriptor);

        wgpu::ShaderSourceWGSL chatWgsl;
        chatWgsl.code = mh::kChatShader;
        wgpu::ShaderModuleDescriptor chatModuleDescriptor{.nextInChain = &chatWgsl};
        wgpu::ShaderModule chatModule = device.CreateShaderModule(&chatModuleDescriptor);

        wgpu::BindGroupLayoutEntry chatLayoutEntry{};
        chatLayoutEntry.binding = 0;
        chatLayoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        chatLayoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;

        wgpu::BindGroupLayoutDescriptor chatLayoutDescriptor{.entryCount = 1, .entries = &chatLayoutEntry};
        wgpu::BindGroupLayout chatBindGroupLayout = device.CreateBindGroupLayout(&chatLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor chatPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                    .bindGroupLayouts = &chatBindGroupLayout};
        wgpu::PipelineLayout chatPipelineLayout = device.CreatePipelineLayout(&chatPipelineLayoutDescriptor);

        wgpu::BlendState chatBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState chatTarget{.format = surfaceFormat, .blend = &chatBlend};
        wgpu::FragmentState chatFragment{
            .module = chatModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &chatTarget};

        wgpu::DepthStencilState chatDepth{.format = kDepthFormat,
                                          .depthWriteEnabled = false,
                                          .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor chatPipelineDescriptor{
            .layout = chatPipelineLayout,
            .vertex = {.module = chatModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &chatDepth,
            .fragment = &chatFragment};
        chatPipeline = device.CreateRenderPipeline(&chatPipelineDescriptor);

        wgpu::BindGroupEntry chatEntry{};
        chatEntry.binding = 0;
        chatEntry.buffer = chatUniformBuffer;
        chatEntry.size = sizeof(ChatUniforms);
        wgpu::BindGroupDescriptor chatBindGroupDescriptor{
            .layout = chatBindGroupLayout, .entryCount = 1, .entries = &chatEntry};
        chatBindGroup = device.CreateBindGroup(&chatBindGroupDescriptor);
    }

    // The death box. Same atlas, same one-triangle-and-discard, and the same
    // three bindings the HUD needs - a uniform block, the glyphs and a
    // sampler for them.
    wgpu::Buffer dialogUniformBuffer;
    wgpu::RenderPipeline dialogPipeline;
    wgpu::BindGroup dialogBindGroup;
    if (fontTexture)
    {
        wgpu::BufferDescriptor dialogBufferDescriptor{
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(DialogUniforms)};
        dialogUniformBuffer = device.CreateBuffer(&dialogBufferDescriptor);

        wgpu::ShaderSourceWGSL dialogWgsl;
        dialogWgsl.code = mh::kDialogShader;
        wgpu::ShaderModuleDescriptor dialogModuleDescriptor{.nextInChain = &dialogWgsl};
        wgpu::ShaderModule dialogModule = device.CreateShaderModule(&dialogModuleDescriptor);

        wgpu::BindGroupLayoutEntry dialogLayoutEntries[3] = {};
        dialogLayoutEntries[0].binding = 0;
        dialogLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        dialogLayoutEntries[1].binding = 1;
        dialogLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        dialogLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        dialogLayoutEntries[2].binding = 2;
        dialogLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        dialogLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor dialogLayoutDescriptor{.entryCount = 3, .entries = dialogLayoutEntries};
        wgpu::BindGroupLayout dialogBindGroupLayout = device.CreateBindGroupLayout(&dialogLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor dialogPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                      .bindGroupLayouts = &dialogBindGroupLayout};
        wgpu::PipelineLayout dialogPipelineLayout = device.CreatePipelineLayout(&dialogPipelineLayoutDescriptor);

        wgpu::BlendState dialogBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState dialogTarget{.format = surfaceFormat, .blend = &dialogBlend};
        wgpu::FragmentState dialogFragment{
            .module = dialogModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &dialogTarget};

        // Over everything, including the water and the names. A modal box that
        // a tree can stand in front of is not modal.
        wgpu::DepthStencilState dialogDepth{.format = kDepthFormat,
                                            .depthWriteEnabled = wgpu::OptionalBool::False,
                                            .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor dialogPipelineDescriptor{
            .layout = dialogPipelineLayout,
            .vertex = {.module = dialogModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &dialogDepth,
            .fragment = &dialogFragment};
        dialogPipeline = device.CreateRenderPipeline(&dialogPipelineDescriptor);

        wgpu::BindGroupEntry dialogEntries[3] = {};
        dialogEntries[0].binding = 0;
        dialogEntries[0].buffer = dialogUniformBuffer;
        dialogEntries[0].size = sizeof(DialogUniforms);
        dialogEntries[1].binding = 1;
        dialogEntries[1].textureView = fontTexture.CreateView();
        dialogEntries[2].binding = 2;
        dialogEntries[2].sampler = fontSampler;
        wgpu::BindGroupDescriptor dialogBindGroupDescriptor{
            .layout = dialogBindGroupLayout, .entryCount = 3, .entries = dialogEntries};
        dialogBindGroup = device.CreateBindGroup(&dialogBindGroupDescriptor);
    }

    float mapCentreX = 0.0f;
    float mapCentreZ = 0.0f;
    float mapHalf = 1.0f;

    if (mapTexture && !collision.empty())
    {
        constexpr uint32_t kMaskSize = 1024;
        const mh::Vec3 middle = zone->centre();
        const float half = std::max(zone->boundsMax.x - zone->boundsMin.x, zone->boundsMax.z - zone->boundsMin.z) * 0.5f;
        mapCentreX = middle.x;
        mapCentreZ = middle.z;
        mapHalf = half;

        const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMaskSize, middle, half);

        wgpu::TextureDescriptor maskDescriptor{.usage = wgpu::TextureUsage::TextureBinding |
                                                        wgpu::TextureUsage::CopyDst,
                                               .dimension = wgpu::TextureDimension::e2D,
                                               .size = {kMaskSize, kMaskSize, 1},
                                               .format = wgpu::TextureFormat::R8Unorm,
                                               .mipLevelCount = 1,
                                               .sampleCount = 1};
        maskTexture = device.CreateTexture(&maskDescriptor);

        wgpu::TexelCopyTextureInfo maskDestination{.texture = maskTexture};
        wgpu::TexelCopyBufferLayout maskLayout{.bytesPerRow = kMaskSize, .rowsPerImage = kMaskSize};
        const wgpu::Extent3D maskExtent{kMaskSize, kMaskSize, 1};
        queue.WriteTexture(&maskDestination, mask.data(), mask.size(), &maskLayout, &maskExtent);

        wgpu::BufferDescriptor radarBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                     .size = sizeof(RadarUniforms)};
        radarUniformBuffer = device.CreateBuffer(&radarBufferDescriptor);

        wgpu::ShaderSourceWGSL radarWgsl;
        radarWgsl.code = mh::kRadarShader;
        wgpu::ShaderModuleDescriptor radarModuleDescriptor{.nextInChain = &radarWgsl};
        wgpu::ShaderModule radarModule = device.CreateShaderModule(&radarModuleDescriptor);

        wgpu::BindGroupLayoutEntry radarLayoutEntries[4] = {};
        radarLayoutEntries[0].binding = 0;
        radarLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        radarLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        radarLayoutEntries[1].binding = 1;
        radarLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[2].binding = 2;
        radarLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[3].binding = 3;
        radarLayoutEntries[3].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[3].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor radarLayoutDescriptor{.entryCount = 4, .entries = radarLayoutEntries};
        wgpu::BindGroupLayout radarBindGroupLayout = device.CreateBindGroupLayout(&radarLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor radarPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                     .bindGroupLayouts = &radarBindGroupLayout};
        wgpu::PipelineLayout radarPipelineLayout = device.CreatePipelineLayout(&radarPipelineLayoutDescriptor);

        wgpu::BlendState radarBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState radarTarget{.format = surfaceFormat, .blend = &radarBlend};
        wgpu::FragmentState radarFragment{
            .module = radarModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &radarTarget};

        // Drawn over everything, so the depth test always passes and nothing is
        // written back.
        wgpu::DepthStencilState radarDepth{.format = kDepthFormat,
                                           .depthWriteEnabled = false,
                                           .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor radarPipelineDescriptor{
            .layout = radarPipelineLayout,
            .vertex = {.module = radarModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &radarDepth,
            .fragment = &radarFragment};
        radarPipeline = device.CreateRenderPipeline(&radarPipelineDescriptor);

        wgpu::BindGroupEntry radarEntries[4] = {};
        radarEntries[0].binding = 0;
        radarEntries[0].buffer = radarUniformBuffer;
        radarEntries[0].size = sizeof(RadarUniforms);
        radarEntries[1].binding = 1;
        radarEntries[1].textureView = mapTexture.CreateView();
        radarEntries[2].binding = 2;
        radarEntries[2].textureView = maskTexture.CreateView();
        radarEntries[3].binding = 3;
        radarEntries[3].sampler = sampler;

        wgpu::BindGroupDescriptor radarBindGroupDescriptor{
            .layout = radarBindGroupLayout, .entryCount = 4, .entries = radarEntries};
        radarBindGroup = device.CreateBindGroup(&radarBindGroupDescriptor);

        std::printf("radar: ready, %zu entities to show\n", options.testEntities.size());
    }

    // `characterPath` is a semicolon-separated list of DATs to assemble
    // one character from, and MOGHOUSE_CHARACTER_AT is where to stand it.
    std::optional<LoadedCharacter> character;
    mh::Vec3 characterAt{};

    // `look` is what a player character actually is:
    // race,face,head,body,hands,legs,feet, all model ids. The skeleton comes
    // from the race and each slot from its own file, which is how a change of
    // outfit is one number rather than a different character.
    if (const char* lookEnv = options.look ? options.look->c_str() : nullptr)
    {
        ffxi::Look look;
        if (!ffxi::parseLook(lookEnv, look))
        {
            std::printf("MOGHOUSE_LOOK wants race,face,head,body,hands,legs,feet\n");
        }
        else
        {
            try
            {
                const ffxi::FileTable table{ffxi::defaultInstallRoot()};
                std::vector<std::string> paths;

                // The skeleton file first: it carries the bones every piece is
                // hung on, and nothing but the race decides it.
                if (auto skeleton = table.path(ffxi::skeletonFileId(look.race)))
                {
                    paths.push_back(skeleton->string());
                }

                // Then the movement motions. Only the first of the four: the
                // rest repeat the same clip names for other weapon stances,
                // and loading them all would just overwrite these.
                const std::vector<size_t> motions = ffxi::motionFileIds(look.race);
                if (!motions.empty())
                {
                    if (auto motion = table.path(motions.front()))
                    {
                        paths.push_back(motion->string());
                    }
                }
                std::printf("look: %s", ffxi::raceName(look.race));
                for (size_t i = 0; i < static_cast<size_t>(ffxi::LookSlot::Count); ++i)
                {
                    const auto slot = static_cast<ffxi::LookSlot>(i);
                    const size_t fileId = ffxi::modelFileId(look.race, slot, look.model[i]);
                    auto path = fileId ? table.path(fileId) : std::nullopt;
                    std::printf(", %s %u%s", ffxi::slotName(slot), look.model[i], path ? "" : " (missing)");
                    if (path)
                    {
                        paths.push_back(path->string());
                    }
                }
                std::printf("\n");
                character = loadCharacter(paths, textures);
            }
            catch (const std::exception& e)
            {
                std::printf("could not read the file table: %s\n", e.what());
            }
        }
    }
    else if (const char* charEnv = options.characterPath ? options.characterPath->c_str() : nullptr)
    {
        std::vector<std::string> paths;
        std::string current;
        for (const char* c = charEnv; *c; ++c)
        {
            if (*c == ';')
            {
                if (!current.empty())
                {
                    paths.push_back(current);
                }
                current.clear();
            }
            else
            {
                current.push_back(*c);
            }
        }
        if (!current.empty())
        {
            paths.push_back(current);
        }
        character = loadCharacter(paths, textures);
    }

    // The character reuses the zone's pipelines and bind group layout, so it
    // needs a zone loaded. Its geometry is already in world orientation, so
    // its one instance is a plain translation rather than a placement matrix.
    wgpu::Buffer characterVertexBuffer;
    // The same geometry frozen in the bind pose, for everyone who is not the
    // player. They shared the player's animated buffer at first, so every NPC
    // in the zone walked in step with whoever was driving - which looks less
    // like a bug than like the whole city being puppeted, but is one.
    //
    // Their own animations need per-entity skinning and a buffer each. Standing
    // still is the honest placeholder until then.
    wgpu::Buffer entityVertexBuffer;
    wgpu::Buffer characterIndexBuffer;
    wgpu::Buffer characterInstanceBuffer;
    std::vector<wgpu::Texture> characterTextures;
    std::vector<wgpu::BindGroup> characterBindGroups;
    if (character && !character->geometry.indices.empty() && pipeline)
    {
        characterVertexBuffer = createBuffer(device, character->geometry.vertices.data(),
                                             character->geometry.vertices.size() * sizeof(mh::Vertex),
                                             wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        characterIndexBuffer = createBuffer(device, character->geometry.indices.data(),
                                            character->geometry.indices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);

        // Taken before the animation loop touches the geometry, so this is the
        // rest pose rather than whatever frame happened to be current.
        mh::reskin(character->geometry, mh::bindPose(character->skeleton), character->meshes);
        entityVertexBuffer = createBuffer(device, character->geometry.vertices.data(),
                                          character->geometry.vertices.size() * sizeof(mh::Vertex),
                                          wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);

        // Slot 0 is the player. The rest are the tracked entities, which
        // share this one skinned mesh: every NPC and every other player is
        // drawn from the same geometry in the same pose, so they cost an
        // instance each and nothing more. Giving them their own models and
        // animations is the next step; standing them in the right places is
        // this one.
        float instance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        wgpu::BufferDescriptor instanceDescriptor{.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                                                  .size = sizeof(instance) * (mh::kMaxDrawnBodies + 1)};
        characterInstanceBuffer = device.CreateBuffer(&instanceDescriptor);
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));

        for (const mh::Batch& batch : character->geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                {
                    characterTextures.push_back(gpu);
                    view = characterTextures.back().CreateView();
                }
            }
            else
            {
                std::printf("character texture %s is in none of those files\n", batch.texture.c_str());
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            characterBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }
    }
    else if (character)
    {
        std::printf("a character needs a zone loaded: it draws with the zone pipelines\n");
    }

    const mh::Vec3 centre = zone ? zone->centre() : mh::Vec3{};
    const float radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;

    mh::Camera camera;
    camera.target = centre;
    camera.distance = radius * 2.4f;
    // Start at the middle of the zone in all three axes. Starting from the
    // bottom of the bounds put the camera 25 units under the terrain in
    // Sarutabaruta, looking at the underside of the world - which reads as the
    // zone being mostly missing rather than as being in the wrong place.
    camera.position = centre;
    camera.pitch = 0.0f;

    // Somewhere visible by default, since nothing yet knows where the ground
    // is. MOGHOUSE_CHARACTER_AT overrides it, and c drops the character at
    // wherever the camera is standing.
    characterAt = centre;
    if (const char* atEnv = options.characterAt ? options.characterAt->c_str() : nullptr)
    {
        std::sscanf(atEnv, "%f,%f,%f", &characterAt.x, &characterAt.y, &characterAt.z);
    }
    // The model faces +x in its own space, so a heading of zero looks east.
    float characterFacing = 0.0f;
    if (const char* facingEnv = options.characterFacing ? options.characterFacing->c_str() : nullptr)
    {
        characterFacing = static_cast<float>(std::atof(facingEnv)) * 3.14159265f / 180.0f;
    }
    // `search` is how far to look for ground when there is none directly
    // below. Dropping a character into a zone wants a wide sweep; a footstep
    // wants none at all. Sharing one value is what let a step off a ledge
    // teleport into the water sixty units away, which is not falling and is
    // not blocking either.
    // Declared up here because writeCharacterInstance below captures them.
    // The bodies are written in the same place as the player's own instance,
    // which is the rule that file already had: nothing else is allowed to
    // produce instance data.
    std::vector<mh::RadarEntity> radarEntities = options.testEntities;
    float radarRange = 120.0f;

    /// How many entity bodies the instance buffer currently holds.
    int drawnBodies = 0;

    /// Where each entity is being drawn, as opposed to where it was last
    /// reported.
    ///
    /// The server sends a position a few times a second and the tracker holds
    /// the last one, so an entity moved in steps: still for four frames, a
    /// jump on the fifth. At sixty frames a second that reads as a tape being
    /// shuttled rather than as somebody walking. Easing towards the reported
    /// position spreads each step over the frames between.
    std::map<uint32_t, mh::Vec3> drawnAt;
    float lastFrameSeconds = 0.0f;

    /// The Vana'diel clock in seconds, when the server has supplied one. Also
    /// gives the weekday, which is the same eight day cycle the game shows.
    uint64_t vanaSeconds = 0;

    /// Where the character has been, sampled about twice a second.
    ///
    /// Collision can still trap someone - a sign post is thin enough that the
    /// step, the slide and both single-axis escapes can all be blocked at
    /// once - and when it does there is nothing to be done from inside the
    /// window. Backing up along a path already known to be walkable is the
    /// cheap way out, and it needs no cleverness about which way is clear.
    std::deque<mh::Vec3> breadcrumbs;
    float breadcrumbTimer = 0.0f;

    /// Collision off: walk through walls and floors, and do not fall.
    ///
    /// The same thing a private server's !wallhack does. Getting somewhere to
    /// look at it should not depend on the collision being right, which is
    /// awkward when the collision is what is being checked.
    bool noclip = false;

    // Typing a line. While this is open the movement keys are letters again,
    // which is the whole reason it is a mode rather than always-on capture.
    bool typing = false;

    /// The keystroke that opened the chat line, which the text-input event for
    /// that same press would otherwise add a second time.
    std::string swallowText;
    std::string typed;

    // Where the character is drawn. Everything that moves them has to call
    // this - the position and the heading only reach the GPU through here, so
    // anything that updates characterAt and forgets leaves the character
    // standing still while the camera follows the place they should be.
    // One built model per distinct look. NPCs repeat heavily - a row of
    // shopkeepers in the same uniform is one model and five instances - so
    // this is keyed by the look rather than by the entity.
    struct DrawableCharacter
    {
        LoadedCharacter loaded;
        wgpu::Buffer vertices;
        wgpu::Buffer indices;
        std::vector<wgpu::Texture> textures;
        std::vector<wgpu::BindGroup> bindGroups;
    };

    std::map<uint64_t, std::optional<DrawableCharacter>> npcModels;

    /// Creatures, in a map of their own.
    ///
    /// Not folded into npcModels under a tagged key: lookKey multiplies the
    /// race by 4096 five times, so a Galka reaches 8 * 2^60 - exactly the top
    /// bit, so any tag up there collides with a real look. Two maps cannot.
    std::map<uint16_t, std::optional<DrawableCharacter>> creatureModels;

    /// One entity's animation, and the vertices it is skinned into.
    ///
    /// The model cache is keyed by look, so a row of identical NPCs shares a
    /// single set of vertices. That is fine while they all stand in the same
    /// pose and useless the moment each needs its own, so every entity keeps
    /// its own copy of the geometry and its own buffer to draw from. Without
    /// it an NPC is a statue being slid around the zone: the server moves it,
    /// nothing bends, and it arrives without ever having taken a step.
    struct AnimatedEntity
    {
        mh::Character geometry;
        wgpu::Buffer vertices;
        const ffxi::Animation* clip = nullptr;
        float clipStart = 0.0f;
        float lastX = 0.0f;
        float lastZ = 0.0f;
        /// The top of the mesh in this frame's pose.
        ///
        /// The model's own bounds are the rest pose and are computed once, so
        /// a clip that lifts the whole body - which is what a bird's flight is
        /// - leaves them describing something on the ground. The nameplate
        /// then sits under a hovering crow instead of over it.
        float posedTop = 0.0f;
        float lastMoveTime = 0.0f;
        float movingUntil = 0.0f;
        float speed = 0.0f;
        bool placed = false;
        bool drawn = false;
    };

    std::map<uint32_t, AnimatedEntity> entityPoses;

    const auto lookKey = [](const uint16_t look[7]) {
        // Race and five equipment slots, packed. Face is left out: it changes
        // the head texture rather than the geometry, and including it would
        // build a separate model for every face in a crowd.
        uint64_t key = look[0];
        for (int slot = 2; slot < 7; ++slot)
        {
            key = key * 4096u + (look[slot] & 0x0FFFu);
        }
        return key;
    };

    // Builds the GPU side of a character. The player's own was built inline
    // above before there was ever more than one; this is the same steps.
    const auto buildDrawable = [&](LoadedCharacter&& loaded) -> std::optional<DrawableCharacter> {
        if (loaded.geometry.indices.empty() || !pipeline)
        {
            return std::nullopt;
        }

        DrawableCharacter drawable;
        drawable.loaded = std::move(loaded);

        // Entities stand in the rest pose. They are not animated individually
        // yet, so there is no per-frame skinning to pay for.
        mh::reskin(drawable.loaded.geometry, mh::bindPose(drawable.loaded.skeleton), drawable.loaded.meshes);

        drawable.vertices = createBuffer(device, drawable.loaded.geometry.vertices.data(),
                                         drawable.loaded.geometry.vertices.size() * sizeof(mh::Vertex),
                                         wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        drawable.indices = createBuffer(device, drawable.loaded.geometry.indices.data(),
                                        drawable.loaded.geometry.indices.size() * sizeof(uint32_t),
                                        wgpu::BufferUsage::Index);

        for (const mh::Batch& batch : drawable.loaded.geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                {
                    drawable.textures.push_back(gpu);
                    view = drawable.textures.back().CreateView();
                }
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            drawable.bindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }

        return drawable;
    };

    // The model for one look, building it the first time it is asked for.
    // Returns null while it has none - a look that resolves to nothing is
    // remembered as nothing rather than retried every frame.
    const auto modelFor = [&](const uint16_t look[7]) -> const DrawableCharacter* {
        const uint64_t key = lookKey(look);
        auto found = npcModels.find(key);
        if (found != npcModels.end())
        {
            return found->second ? &*found->second : nullptr;
        }

        ffxi::Look wanted;
        wanted.race = static_cast<ffxi::Race>(look[0]);
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Face)] = look[1];
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Head)] = look[2] & 0x0FFF;
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Body)] = look[3] & 0x0FFF;
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Hands)] = look[4] & 0x0FFF;
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Legs)] = look[5] & 0x0FFF;
        wanted.model[static_cast<size_t>(ffxi::LookSlot::Feet)] = look[6] & 0x0FFF;

        std::optional<DrawableCharacter> built;
        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            std::vector<std::string> paths;
            if (auto skeletonPath = table.path(ffxi::skeletonFileId(wanted.race)))
            {
                paths.push_back(skeletonPath->string());
            }
            for (const std::filesystem::path& piece : ffxi::lookFiles(table, wanted))
            {
                paths.push_back(piece.string());
            }

            if (!paths.empty())
            {
                if (auto loaded = loadCharacter(paths, textures))
                {
                    built = buildDrawable(std::move(*loaded));
                }
            }
        }
        catch (const std::exception& e)
        {
            std::printf("could not build NPC model: %s\n", e.what());
        }

        std::printf("NPC model %s: race %u %u/%u/%u/%u/%u\n", built ? "built" : "failed", look[0], look[2],
                    look[3], look[4], look[5], look[6]);
        auto inserted = npcModels.emplace(key, std::move(built)).first;
        return inserted->second ? &*inserted->second : nullptr;
    };

    /// A creature's model, cached by the id the server sent.
    ///
    /// Nothing to assemble: one file holds the skeleton, the mesh and the
    /// animations, and the clips are named the way a player's are, so the same
    /// idl0/wlk0/run0 the rest of the renderer looks for is a rabbit sitting,
    /// hopping and running without any special case.
    const auto creatureFor = [&](uint16_t modelId) -> const DrawableCharacter* {
        auto found = creatureModels.find(modelId);
        if (found != creatureModels.end())
        {
            return found->second ? &*found->second : nullptr;
        }

        std::optional<DrawableCharacter> built;
        try
        {
            const ffxi::FileTable table{ffxi::defaultInstallRoot()};
            if (auto path = table.path(mh::creatureFileId(modelId)))
            {
                if (auto loaded = loadCharacter({path->string()}, textures))
                {
                    built = buildDrawable(std::move(*loaded));
                }
            }
        }
        catch (const std::exception& e)
        {
            std::printf("could not build creature %u: %s\n", modelId, e.what());
        }

        std::printf("creature model %s: id %u -> file %zu\n", built ? "built" : "failed", modelId,
                    mh::creatureFileId(modelId));
        auto inserted = creatureModels.emplace(modelId, std::move(built)).first;
        return inserted->second ? &*inserted->second : nullptr;
    };

    /// Whichever way this entity is described.
    const auto modelForEntity = [&](const mh::RadarEntity& entity) -> const DrawableCharacter* {
        if (entity.hasModel())
        {
            return creatureFor(entity.modelId);
        }
        return entity.hasLook() ? modelFor(entity.look) : nullptr;
    };

    auto writeCharacterInstance = [&]() {
        if (!characterInstanceBuffer)
        {
            return;
        }
        // characterFacing is a compass heading: 0 is +z, and the direction is
        // (sin, cos) - the same convention camera.yaw and the radar notch use.
        // The model faces +x when unrotated, so the rotation that points it
        // along the heading is a quarter turn less.
        //
        // Less, not more. The matrix below sends local +x to
        // (cos turn, 0, -sin turn); for that to equal (sin f, 0, cos f) the
        // turn has to be f - pi/2. With the quarter turn the other way round
        // the z component comes out negated, which is the same mistake the
        // heading reported to the server had: correct only when facing due
        // east or west, backwards when facing north, and reading as a
        // character that turns the wrong way as it walks.
        const float turn = characterFacing - 1.57079633f;
        const float c = std::cos(turn);
        const float sn = std::sin(turn);
        const float instance[16] = {c,  0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0,
                                    characterAt.x, characterAt.y, characterAt.z, 1};
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));

        // And one for each tracked entity, in the same buffer behind the
        // player. Written here rather than in the frame loop because this is
        // the only place the instance data is allowed to be produced - the
        // comment above says so, and the last time something updated a
        // position without coming through here the character stopped moving.
        drawnBodies = 0;
        for (const mh::RadarEntity& entity : radarEntities)
        {
            if (drawnBodies >= mh::kMaxDrawnBodies)
            {
                break;
            }
            const float bodyTurn = entity.heading - 1.57079633f;
            const float bc = std::cos(bodyTurn);
            const float bs = std::sin(bodyTurn);
            const float body[16] = {bc, 0, -bs, 0, 0, 1, 0, 0, bs, 0, bc, 0,
                                    entity.x, entity.y, entity.z, 1};
            queue.WriteBuffer(characterInstanceBuffer, sizeof(body) * (drawnBodies + 1), body, sizeof(body));
            ++drawnBodies;
        }
    };

    auto placeCharacter = [&](const mh::Vec3& where, float search) {
        // Copied, not referenced. Every caller so far happened to pass
        // characterAt itself, which made the aliasing invisible; the one that
        // does not - dropping the character at the camera - would have written
        // the unsnapped height.
        const mh::Vec3 target = where;
        characterAt = target;
        if (const std::optional<mh::Vec3> ground =
                collision.nearestGround(target.x, target.z, target.y + 1.0f, search))
        {
            // Snapping up is what a caller wants: a spawn point given a
            // slightly-too-low height belongs on the floor above it. Snapping
            // *down* is not - a position well clear of the ground is a request
            // to be in the air, and gravity is a better answer to that than a
            // teleport.
            characterAt = target.y > ground->y + 1.0f ? mh::Vec3{ground->x, target.y, ground->z} : *ground;
        }
        writeCharacterInstance();
    };
    placeCharacter(characterAt, 60.0f);
    if (character)
    {
        std::printf("character stands at %.1f %.1f %.1f%s\n", characterAt.x, characterAt.y, characterAt.z,
                    collision.empty() ? " (no collision - movement unconstrained)" : "");
    }

    // Framing a shot from a script needs the camera to be settable; dragging
    // it into place by hand cannot be repeated.
    if (const char* cameraEnv = options.camera ? options.camera->c_str() : nullptr)
    {
        std::sscanf(cameraEnv, "%f,%f,%f", &camera.position.x, &camera.position.y, &camera.position.z);
    }
    if (const char* lookEnv = options.cameraLook ? options.cameraLook->c_str() : nullptr)
    {
        // yaw,pitch in degrees, and optionally how far back to orbit from.
        // The default hundred units surveys a whole zone, which is far too far
        // away to check anything about the character itself.
        float yawDegrees = 0.0f;
        float pitchDegrees = 0.0f;
        float orbitDistance = 0.0f;
        const int given = std::sscanf(lookEnv, "%f,%f,%f", &yawDegrees, &pitchDegrees, &orbitDistance);
        camera.yaw = yawDegrees * 3.14159265f / 180.0f;
        camera.pitch = pitchDegrees * 3.14159265f / 180.0f;
        if (given >= 3 && orbitDistance > 0.0f)
        {
            camera.distance = orbitDistance;
        }
    }

    // Two cursors. The pointer says whether there is anything under it
    // worth clicking, which is the feedback a click wants before it happens
    // rather than after.
    SDL_Cursor* arrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    SDL_Cursor* handCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    // The zone's music. Opened here rather than at startup so a client
    // that cannot make a sound still draws.
    mh::Music music;

    /// Kept here as well as in the device so the keys have something to step.
    /// Starts low: music you have to turn down is worse than music you have to
    /// turn up, and this one starts the moment you log in.
    float musicVolume = 0.35f;
    if (const char* volume = std::getenv("MOGHOUSE_MUSIC_VOLUME"))
    {
        musicVolume = std::clamp(static_cast<float>(std::atof(volume)), 0.0f, 1.0f);
    }
    music.setVolume(musicVolume);

    bool dragging = false;

    /// Whether the pointer moved between press and release. A drag turns the
    /// camera; a click picks a target, and they start out identical.
    bool dragMoved = false;

    std::printf("wasd to walk, mouse drag to look, space to jump, wheel or numpad 9/3 to zoom,\n");
    std::printf("shift to run, r to auto-run, tab to orbit, p to print position,\n");
    std::printf("c to place the character,\n");
    std::printf("u to back up the trail if collision traps you, n for no collision,\n");
    std::printf("numpad 8/2 to move and 4/6 to turn, numpad minus to walk, shift to invert it,\n");
    std::printf("f to swap between driving the character and flying the camera,\n");
    std::printf("escape to quit\n");

    // `screenshotPath` writes one frame to a BMP and quits. Without it
    // there is no way to check what the renderer actually produced except by
    // looking at the window, which rules out checking anything unattended.
    const char* screenshotPath = options.screenshotPath ? options.screenshotPath->c_str() : nullptr;

    // `screenshotSequence` captures a whole animation in one run,
    // one file per source frame, with the path taken as a printf format. The
    // alternative is relaunching for each frame, which for a walk cycle is
    // eighteen launches of a program that spends most of its time loading a
    // zone.
    const int sequenceCount = options.screenshotSequence;
    int shotIndex = -options.settleFrames; // let the first frames settle
    wgpu::Buffer readbackBuffer;

    // `animation` names one of the character's own animations - idl0
    // for a standing idle, wlk0 to walk, run0 to run. Skinning runs on the CPU
    // and the vertices are rewritten each frame: a couple of thousand
    // triangles is nothing next to a zone, and it keeps the pose maths
    // somewhere it can be read.
    const ffxi::Animation* playing = nullptr;
    const ffxi::Animation* idleClip = nullptr;
    const ffxi::Animation* walkClip = nullptr;
    const ffxi::Animation* runClip = nullptr;
    const ffxi::Animation* jumpClip = nullptr;
    const ffxi::Animation* deadClip = nullptr;
    // When the jump finishes, on the same clock animationOffset is measured
    // against. Idle, walk and run are chosen every frame from what the
    // character is doing; a jump is not, so it needs an end to hold until.
    float jumpUntil = 0.0f;
    std::function<const ffxi::Animation*(const ffxi::Animation*)> upperFor;
    // MOGHOUSE_ANIMATION pins one clip; without it, movement picks.
    const bool pinnedClip = options.animation ? options.animation->c_str() : nullptr != nullptr;
    float animationOffset = 0.0f;
    bool driving = character.has_value();

    if (character && !character->animations.empty())
    {
        auto find = [&](const char* name) -> const ffxi::Animation* {
            auto found = character->animations.find(name);
            return found == character->animations.end() ? nullptr : &found->second;
        };
        idleClip = find("idl0");
        walkClip = find("wlk0");
        runClip = find("run0");
        jumpClip = find("jmp0");
        deadClip = find("ded0");

        // The clips ending 0 drive the root, hips and legs - sixteen bones of
        // ninety-four. Everything above the waist, the spine and torso and
        // arms and a Mithra or Galka tail, lives in the clips ending 1, and
        // movement has no 1 of its own. std1 is the standing upper body, and
        // layering it under a stride is what gets the arms swinging.
        // A clip's upper body is its own name with the trailing 0 turned into
        // a 1: wlk0 walks the legs and wlk1 swings the arms above them. They
        // are stored apart because the upper half depends on what the
        // character is holding, so the two are chosen separately and played
        // together.
        //
        // MOGHOUSE_UPPER pins one for every clip, or "none" to go back to the
        // legs alone.
        const char* upperPin = std::getenv("MOGHOUSE_UPPER");
        upperFor = [&character, upperPin](const ffxi::Animation* lower) -> const ffxi::Animation* {
            if (upperPin && std::strcmp(upperPin, "none") == 0)
            {
                return nullptr;
            }
            std::string name = upperPin ? upperPin : (lower ? lower->name : std::string{});
            if (!upperPin)
            {
                if (name.empty() || name.back() != '0')
                {
                    return nullptr;   // already an upper body, or unpaired
                }
                name.back() = '1';
            }
            auto found = character->animations.find(name);
            return found == character->animations.end() ? nullptr : &found->second;
        };

        // How much of the skeleton each clip actually drives. A clip that only
        // carries tracks for the legs leaves the arms in the bind pose, which
        // reads as a character running with its arms held still.
        if (std::getenv("MOGHOUSE_ANIMATION_TRACKS"))
        {
            for (const auto& [clipName, clip] : character->animations)
            {
                std::printf("  %-6s %3u frames, %zu of %zu bones driven", clipName.c_str(), clip.frames,
                            clip.tracks.size(), character->skeleton.bones.size());
                if (std::strcmp(std::getenv("MOGHOUSE_ANIMATION_TRACKS"), "bones") == 0)
                {
                    std::printf("   bones:");
                    for (const ffxi::AnimationTrack& track : clip.tracks)
                    {
                        std::printf(" %u", track.bone);
                    }
                }
                std::printf("\n");
            }
        }

        const char* wanted = options.animation ? options.animation->c_str() : nullptr;
        auto found = character->animations.find(wanted ? wanted : "idl0");
        if (found == character->animations.end() && wanted)
        {
            std::printf("no animation called %s; this character has:", wanted);
            for (const auto& [name, unused] : character->animations)
            {
                std::printf(" %s", name.c_str());
            }
            std::printf("\n");
        }
        if (found != character->animations.end())
        {
            playing = &found->second;
            std::printf("playing %s: %u frames at %.1f a second\n", playing->name.c_str(), playing->frames,
                        1.0f / playing->frameSeconds());
        }
    }

    // `frame` pins the animation clock so a screenshot of a moving
    // character lands on the same pose every time.
    // What the radar shows. Fed from options until something is connected,
      // and replaced wholesale rather than merged - the list is already the
      // answer to "what can be seen right now".

    // The zone name as glyph indices, resolved once. Anything outside the font
    // becomes a space rather than a wrong letter, and underscores read as
    // spaces because that is how the server names zones.
    std::vector<int> labelIndices;
    if (options.zoneName)
    {
        static const std::string kOrder = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-.";
        for (char raw : *options.zoneName)
        {
            if (labelIndices.size() >= mh::kRadarMaxLabel)
            {
                break;
            }
            char upper = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
            if (upper == '_')
            {
                upper = ' ';
            }
            const size_t found = kOrder.find(upper);
            labelIndices.push_back(found == std::string::npos ? 0 : static_cast<int>(found));
        }
    }

    /// How fast a fall accelerates, in units per second squared. Not measured
    /// against anything - FFXI's own value is not in the DATs - so it is
    /// tuned to look right at this scale, where a character is 1.8 units tall.
    /// How deep the water may be and still be walked into. Ankle-deep puddles
/// and fountain rims are walkable in FFXI; a canal is not.
constexpr float kWadeDepth = 1.2f;

/// How far a single step may drop before it is refused.
///
/// Generous enough for a stair tread or a kerb, short enough that walking off
/// a quay or into a gap in the geometry stops at the edge.
constexpr float kMaxStepDown = 1.6f;

constexpr float kGravity = 26.0f;

    /// How far below the feet still counts as standing on something. Slopes
    /// and the discrete steps of a walk both leave small gaps, and treating
    /// those as falling makes a character jitter downhill.
    constexpr float kGroundSnap = 0.25f;

    /// How far down to look for the floor while falling.
    constexpr float kFallReach = 500.0f;

    float fallSpeed = 0.0f;

    const float pinnedFrame = options.frame.value_or(-1.0f);
    const float shaderMode = options.shaderMode;
    const int cutoutMode = options.cutoutMode;

    // `timeOfDay` = 1830 pins the clock; otherwise a Vana'diel day passes in
    // one real minute, which is fast but makes the whole cycle visible.
    const bool timeFixed = options.timeOfDay.has_value();
    const int fixedMinutes = timeFixed ? (*options.timeOfDay / 100) * 60 + (*options.timeOfDay % 100) : 0;
    if (!lighting.empty())
    {
        std::printf("lighting: %zu times of day, clock %s\n", lighting.sets().size(),
                    timeFixed ? "fixed" : "running");
    }

    // Run unless told otherwise, which is what the game does - there is no
    // walking anywhere by accident. Numpad minus toggles it, the way the real
    // client does, and holding shift inverts whichever way the toggle is set,
    // so a moment of walking does not need a mode change and back.
    bool walkByDefault = false;

    /// Keep going forward without holding the key. R toggles it, and anything
    /// that means "stop" clears it: pressing back, or dying. Holding forward
    /// while it is on is harmless - it is the same direction.
    bool autoRun = false;

    /// Whether the radar turns with the player or holds north at the top.
    ///
    /// Both are defensible and people are firm about which they want, so it is
    /// a toggle rather than a decision. M switches it; MOGHOUSE_RADAR_NORTH
    /// picks the one you start with.
    bool radarTurns = std::getenv("MOGHOUSE_RADAR_NORTH") == nullptr;

    // The death box, as the last frame left it.
    //
    // Immediate mode: the box is laid out while it is drawn and the rectangles
    // are kept, so a click is tested against the frame the player was looking
    // at when they pressed. A frame's lag on a box that only appears when a
    // character has stopped moving is not something anyone can aim past.
    bool deathBoxShown = false;
    float deathPanel[4]{};
    DialogButton deathButtons[mh::kDialogButtons]{};

    /// The two chips in the top left corner, as the last frame drew them.
    ///
    /// Somewhere to send a bug from inside the game, rather than from the
    /// launcher the player cannot see while the world is up.
    struct CornerLink
    {
        float left{}, bottom{}, width{}, height{};
        mh::ViewerLink::Link target{mh::ViewerLink::Link::None};

        bool holds(float x, float y) const
        {
            return width > 0.0f && x >= left && x < left + width && y >= bottom && y < bottom + height;
        }
    };
    CornerLink cornerLinks[2]{};

    // Which button the mouse went down on, so releasing somewhere else is a
    // change of mind rather than a press. -1 is none.
    int deathPressed = -1;

    // A pointer position in the coordinates the box is laid out in.
    //
    // SDL reports window points and the surface is measured in pixels, which
    // are not the same number on a high density display - so this divides by
    // the window rather than by the frame. What the two share is the aspect,
    // and the aspect is all normalised device coordinates need.
    /// The last frame's view projection, for turning a click into a target.
    ///
    /// Picking needs to know where things were on screen, and the only place
    /// that knows is the draw. Keeping the matrix rather than recomputing it
    /// means the cursor is tested against exactly the frame that was shown.
    /// Who was last clicked, and who the cursor is over.
    ///
    /// FFXI draws a ring under the thing you have targeted, which is both the
    /// clearest indication and the cheapest: the zone line pipeline already
    /// draws a glowing ring at a world position, so a target is one more ring
    /// in a different colour rather than a second way of drawing.
    uint32_t targetId = 0;
    uint32_t hoverId = 0;

    mh::Mat4 pickProjection{};
    bool havePickProjection = false;

    /// Who is under the cursor, or 0.
    ///
    /// Entities are projected to the screen rather than the cursor being
    /// unprojected into a ray: the bodies are already drawn from a position
    /// and a height, so a point and a radius describes them as well as a
    /// volume would, and it needs no matrix inversion. Nearest to the camera
    /// wins among those the cursor is over, which is what makes clicking a
    /// shopkeeper standing in front of a wall pick the shopkeeper.
    const auto pickAt = [&](float ndcX, float ndcY) -> uint32_t {
        if (!havePickProjection)
        {
            return 0;
        }

        uint32_t best = 0;
        float bestDepth = 0.0f;
        for (const mh::RadarEntity& entity : radarEntities)
        {
            // Anything with a body, whether or not the server flagged it.
            //
            // 0x28 bit 0x40 looked like the answer and is not, at least on
            // LandSandBoat: setTriggerable(true) is called from one place, on
            // the path that builds an NPC from a Lua table, and only when an
            // onTrigger is passed there. NPCs loaded from a zone's dataset
            // never get it however many scripts they have - Windurst Waters
            // reported 0 of 28 - while mobs do. Filtering on it made monsters
            // clickable and shopkeepers not, which is backwards.
            //
            // The flag is still read and carried; it is just not a gate. What
            // makes something worth clicking is having a body and not being
            // hidden, and the server's own reply settles the rest.
            if (!entity.hasLook() && !entity.hasModel())
            {
                continue;
            }

            const DrawableCharacter* model = modelForEntity(entity);
            const float height = model ? model->loaded.geometry.height() : 1.8f;

            const float world[4] = {entity.x, entity.y + height * 0.5f, entity.z, 1.0f};
            const float* m = pickProjection.m;
            const float clipX = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
            const float clipY = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
            const float clipW = m[3] * world[0] + m[7] * world[1] + m[11] * world[2] + m[15];
            if (clipW <= 0.01f)
            {
                continue;   // behind the camera
            }

            const float screenX = clipX / clipW;
            const float screenY = clipY / clipW;

            // How big the body is on screen, so a distant NPC is a small
            // target and a near one is a generous one - the same way it looks.
            const float onScreen = std::max(height / clipW, 0.02f);
            const float dx = screenX - ndcX;
            const float dy = (screenY - ndcY) * 0.5f;   // NDC y is not square with x
            if (dx * dx + dy * dy > onScreen * onScreen)
            {
                continue;
            }

            if (best == 0 || clipW < bestDepth)
            {
                best = entity.id;
                bestDepth = clipW;
            }
        }

        return best;
    };

    const auto pointerNdc = [window](float x, float y, float& ndcX, float& ndcY)
    {
        int pointsAcross = 0;
        int pointsDown = 0;
        SDL_GetWindowSize(window, &pointsAcross, &pointsDown);
        if (pointsAcross <= 0 || pointsDown <= 0)
        {
            return false;
        }
        ndcX = x / static_cast<float>(pointsAcross) * 2.0f - 1.0f;
        ndcY = 1.0f - y / static_cast<float>(pointsDown) * 2.0f;
        return true;
    };

    uint64_t previousTicks = SDL_GetTicksNS();
    bool running = true;
    while (running)
    {
        // Dead, and whether a way up has been offered. Read once at the top of
        // the frame and answered everywhere below: a corpse does not walk and
        // does not jump, it holds the fallen pose rather than the idle one,
        // and it is looking at the box that asks what to do about it.
        //
        // One read rather than one per site, because one per site drifted.
        // Only the box knew about testDeath, so the standalone viewer drew it
        // over a character still standing up - four parts of one state
        // disagreeing about whether anyone had died.
        bool raiseOffered = options.testDeath > 1;
        const bool dead = link ? link->dead(raiseOffered) : options.testDeath > 0;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            else if (event.type == SDL_EVENT_TEXT_INPUT && typing)
            {
                if (!swallowText.empty())
                {
                    const bool sameKey = event.text.text && swallowText == event.text.text;
                    swallowText.clear();
                    if (sameKey)
                    {
                        continue;
                    }
                }
                typed += event.text.text;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && typing)
            {
                // While typing, the keyboard belongs to the line and nothing
                // else - or w a s d walk the character out from under you
                // mid-sentence.
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                {
                    if (!typed.empty() && link)
                    {
                        link->submitChat(typed);
                        link->pushChat("> " + typed);
                    }
                    typed.clear();
                    typing = false;
                    SDL_StopTextInput(window);
                }
                else if (event.key.key == SDLK_ESCAPE)
                {
                    typed.clear();
                    typing = false;
                    SDL_StopTextInput(window);
                }
                else if (event.key.key == SDLK_BACKSPACE && !typed.empty())
                {
                    typed.pop_back();
                }
            }
            else if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                {
                    // Return opens the line, the way the game does. Only when
                    // there is a client to say it to.
                    if (link)
                    {
                        typing = true;
                        typed.clear();
                        SDL_StartTextInput(window);
                    }
                }
                else if (link && (event.key.key == SDLK_SLASH || event.key.key == SDLK_EXCLAIM))
                {
                    // Almost everything typed here starts with one of these -
                    // / for the client's own commands, ! for the server's - so
                    // they open the line and put themselves in it rather than
                    // opening an empty one to type them into.
                    typing = true;
                    typed = event.key.key == SDLK_SLASH ? "/" : "!";
                    SDL_StartTextInput(window);

                    // The character that opened the line arrives again as text
                    // input a moment later, and would be doubled.
                    swallowText = typed;
                }
                else if (event.key.key == SDLK_ESCAPE)
                {
                    running = false;
                }
                else if (event.key.key == SDLK_TAB)
                {
                    camera.orbiting = !camera.orbiting;
                }
                else if (event.key.key == SDLK_MINUS || event.key.key == SDLK_EQUALS ||
                         event.key.key == SDLK_PLUS)
                {
                    // Minus and equals, because equals is the unshifted plus
                    // and nobody holds shift to turn music up.
                    const float step = event.key.key == SDLK_MINUS ? -0.05f : 0.05f;
                    musicVolume = std::clamp(musicVolume + step, 0.0f, 1.0f);
                    music.setVolume(musicVolume);
                    std::printf("music volume %.0f%%\n", musicVolume * 100.0f);
                }
                else if (event.key.key == SDLK_M)
                {
                    radarTurns = !radarTurns;
                    std::printf(radarTurns ? "radar turns with you\n" : "radar holds north up\n");
                }
                else if (event.key.key == SDLK_R)
                {
                    autoRun = !autoRun;
                    std::printf(autoRun ? "auto-run on\n" : "auto-run off\n");
                }
                // Deliberately not gated on being dead.
                //
                // A corpse cannot walk, act or use an ability - the server
                // turns all of it away - so a dead player has no way to say "I
                // am over here, and I would like a raise". Two things do still
                // get through, and this key sends both.
                //
                // Alive it is a jump. Dead it is a wave and a shove: the client
                // turns the request into an emote (see FfxiGameSession's
                // JumpAsync), and the body drags itself a little way along the
                // ground here, which the client reports like any other movement.
                // LandSandBoat relays both - its jump, motion and position
                // handlers all check only that you are not mid-event, never
                // that you are alive. See 0x11d_jump.cpp, 0x05d_motion.cpp and
                // 0x015_pos.cpp.
                //
                // What it looks like from here is a twitch rather than a hop:
                // the pose below overrules the jump clip on this same frame and
                // restarts the death clip, so the body flinches where it lies.
                // jumpUntil rate-limits it to one per clip, so it cannot be
                // held down into a seizure.
                else if (event.key.key == SDLK_SPACE && driving && jumpClip)
                {
                    // Only while driving: flying the camera, space is still
                    // up. Restarting mid-jump would reset the clip on every
                    // repeat of a held key, so an unfinished one is left to
                    // land first.
                    const float now = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
                    if (jumpUntil <= now)
                    {
                        if (dead)
                        {
                            // Rewound to the last few frames of the death clip,
                            // never restarted and never handed to the jump.
                            //
                            // Either of those puts the clip back at frame zero,
                            // and frame zero of a death is the character on
                            // their feet - so a press stood the corpse up and
                            // dropped it again, which is a resurrection, not a
                            // twitch. The tail is the body's last settle onto
                            // the ground, which is exactly the movement wanted.
                            if (deadClip && deadClip->frames > kCorpseTwitchFrames)
                            {
                                animationOffset =
                                    now - static_cast<float>(deadClip->frames - kCorpseTwitchFrames) *
                                              deadClip->frameSeconds();
                            }

                            // And the drag. Half a walking pace, along the way
                            // the body already faces, through the same
                            // collision check a step takes - a corpse hauling
                            // itself through a wall would be worse than one
                            // that cannot move at all. The height is left to
                            // the fall below, which settles it back down.
                            if (character)
                            {
                                const mh::Vec3 wanted{characterAt.x + std::sin(characterFacing) * kCorpseDrag,
                                                      characterAt.y,
                                                      characterAt.z + std::cos(characterFacing) * kCorpseDrag};
                                characterAt = collision.empty() || noclip
                                                  ? wanted
                                                  : collision.move(characterAt, wanted, 0.5f);
                            }
                        }
                        else
                        {
                            playing = jumpClip;
                            animationOffset = now;
                        }

                        jumpUntil = now + static_cast<float>(jumpClip->frames) * jumpClip->frameSeconds();

                        // And tell the client, which is the only half of this
                        // that talks to the server. Without it the jump - or
                        // the wave it becomes when dead - is ours alone and
                        // nobody else ever sees it.
                        if (link)
                        {
                            link->requestJump();
                        }
                    }
                }
                else if (event.key.key == SDLK_E && driving && link)
                {
                    // Talk to whoever is closest in front.
                    //
                    // The real client targets first and acts second; this is
                    // the short version, because a target you cannot see the
                    // name of is not worth the extra step yet. In front rather
                    // than merely near, so standing between two NPCs picks the
                    // one being faced instead of whichever happens to be a few
                    // centimetres closer.
                    const float facing = camera.yaw;
                    const float aheadX = -std::sin(facing);
                    const float aheadZ = -std::cos(facing);

                    uint32_t chosen = 0;
                    float best = 0.0f;
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        const float dx = entity.x - characterAt.x;
                        const float dz = entity.z - characterAt.z;
                        const float distance = std::sqrt(dx * dx + dz * dz);
                        if (distance < 0.01f || distance > 6.0f)
                        {
                            continue;
                        }

                        // How squarely it is in front, as a cosine.
                        const float towards = (dx / distance) * aheadX + (dz / distance) * aheadZ;
                        if (towards < 0.5f)
                        {
                            continue;   // off to the side or behind
                        }

                        const float score = towards / distance;
                        if (score > best)
                        {
                            best = score;
                            chosen = entity.id;
                        }
                    }

                    if (chosen != 0)
                    {
                        link->requestTalk(chosen);
                    }
                }
                else if (event.key.key == SDLK_KP_MINUS)
                {
                    walkByDefault = !walkByDefault;
                    std::printf("%s\n", walkByDefault ? "walking" : "running");
                }
                else if (event.key.key == SDLK_P)
                {
                    const mh::Vec3 at = camera.eye();
                    std::printf("at %.1f %.1f %.1f   zone y runs %.1f to %.1f\n", at.x, at.y, at.z,
                                zone->boundsMin.y, zone->boundsMax.y);
                }
                else if (event.key.key == SDLK_N)
                {
                    noclip = !noclip;
                    fallSpeed = 0.0f;
                    std::printf("collision %s\n", noclip ? "off - walking through walls" : "on");
                }
                else if (event.key.key == SDLK_U && character)
                {
                    // Back up the trail. Three samples is a second and a half
                    // of walking, which clears anything that catches a corner
                    // without throwing away where you were going.
                    for (int back = 0; back < 3 && breadcrumbs.size() > 1; ++back)
                    {
                        breadcrumbs.pop_back();
                    }
                    if (!breadcrumbs.empty())
                    {
                        placeCharacter(breadcrumbs.back(), 20.0f);
                        std::printf("unstuck to %.1f %.1f %.1f\n", characterAt.x, characterAt.y, characterAt.z);
                    }
                }
                else if (event.key.key == SDLK_F && character)
                {
                    driving = !driving;
                    camera.orbiting = driving;
                    if (!driving)
                    {
                        camera.position = camera.eye();
                    }
                    std::printf("%s\n", driving ? "driving the character" : "flying the camera");
                }
                else if (event.key.key == SDLK_C && character)
                {
                    // Stand the character where the camera is. Nothing knows
                    // where the ground is yet, so putting it somewhere useful
                    // is a matter of walking there and pressing a key.
                    const mh::Vec3 at = camera.eye();
                    placeCharacter(at, 60.0f);
                    std::printf("character moved to %.1f %.1f %.1f\n", at.x, at.y, at.z);
                }
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                configure(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                const bool onBox = deathBoxShown && pointerNdc(event.button.x, event.button.y, ndcX, ndcY) &&
                                   ndcX >= deathPanel[0] && ndcX < deathPanel[0] + deathPanel[2] &&
                                   ndcY >= deathPanel[1] && ndcY < deathPanel[1] + deathPanel[3];

                deathPressed = -1;
                if (onBox)
                {
                    for (int i = 0; i < mh::kDialogButtons; ++i)
                    {
                        if (deathButtons[i].enabled && deathButtons[i].holds(ndcX, ndcY))
                        {
                            deathPressed = i;
                        }
                    }
                }

                // A press on the box belongs to the box. Otherwise the same
                // press also grabs the camera, and answering the one question
                // a dead character is allowed to answer swings the view round
                // while you do it.
                dragging = !onBox;
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            {
                // A press and release without much movement is a click; with
                // movement it was the camera being turned. Without that
                // distinction every look-around ends by talking to whoever the
                // cursor happened to land on.
                float upX = 0.0f;
                float upY = 0.0f;
                if (!dragMoved && link && pointerNdc(event.button.x, event.button.y, upX, upY))
                {
                    for (const CornerLink& chip : cornerLinks)
                    {
                        if (chip.target != mh::ViewerLink::Link::None && chip.holds(upX, upY))
                        {
                            link->chooseLink(chip.target);
                            dragging = false;
                            break;
                        }
                    }
                }

                if (dragging && !dragMoved && link && pointerNdc(event.button.x, event.button.y, upX, upY))
                {
                    if (uint32_t clicked = pickAt(upX, upY))
                    {
                        targetId = clicked;
                        link->requestTalk(clicked);
                    }
                    else
                    {
                        targetId = 0;   // clicking the ground clears the target
                    }
                }

                dragging = false;
                dragMoved = false;

                // Pressed and released on the same button. Sliding off one
                // before letting go is how a mis-click is taken back, which
                // every other program on the machine already agrees about.
                float ndcX = 0.0f;
                float ndcY = 0.0f;
                if (deathPressed >= 0 && deathBoxShown && link &&
                    pointerNdc(event.button.x, event.button.y, ndcX, ndcY) &&
                    deathButtons[deathPressed].enabled && deathButtons[deathPressed].holds(ndcX, ndcY))
                {
                    link->chooseDeath(deathButtons[deathPressed].choice);
                }
                deathPressed = -1;
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && !dragging)
            {
                // What the cursor is over, so it can say so before a click.
                float overX = 0.0f;
                float overY = 0.0f;
                hoverId = pointerNdc(event.motion.x, event.motion.y, overX, overY) ? pickAt(overX, overY) : 0;
                SDL_SetCursor(hoverId != 0 ? handCursor : arrowCursor);
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && dragging)
            {
                // A few pixels of slack, because a hand on a mouse is never
                // perfectly still and a click that turns the camera a hair
                // should still count as a click.
                if (std::fabs(event.motion.xrel) + std::fabs(event.motion.yrel) > 3.0f)
                {
                    dragMoved = true;
                }

                camera.look(-event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                // Driving a character, the camera lives in the same 1.5 to 25
                // as the numpad keys. Flying, it is surveying a whole zone and
                // wants the zone's own scale. Sharing one floor meant the
                // wheel stopped a long way short of the character in a city,
                // where a twentieth of the radius is still tens of units.
                if (driving)
                {
                    wantedDistance = std::clamp(wantedDistance * (event.wheel.y > 0 ? 0.9f : 1.1f), 1.5f, 25.0f);
                    camera.distance = wantedDistance;
                }
                else
                {
                    camera.distance = std::clamp(camera.distance * (event.wheel.y > 0 ? 0.9f : 1.1f),
                                                 radius * 0.05f, radius * 12.0f);
                }
            }
        }

        // The server's clock when it gave us one, our own otherwise.
        //
        // Vana'diel runs 25 times real time - a minute of it is 2.4 seconds -
        // so the seed only has to arrive once and then be advanced. Without
        // this the renderer invented a day at its own rate, and two clients
        // side by side showed different hours and different light, which is
        // precisely when you want them to agree.
        int clockMinutes = 0;
        if (timeFixed)
        {
            clockMinutes = fixedMinutes;
        }
        else if (options.serverClock)
        {
            // The server sends Earth seconds since the Vana'diel epoch, not
            // Vana'diel seconds - earth_time.h says so in as many words - so
            // the seed is multiplied up rather than used as it stands. Without
            // the 25 the clock both starts in the wrong place and then runs at
            // a twenty-fifth of the right speed.
            const uint64_t elapsed = SDL_GetTicksNS() / 1000000000ull;
            vanaSeconds = (static_cast<uint64_t>(*options.serverClock) + elapsed) * 25ull;
            clockMinutes = static_cast<int>((vanaSeconds / 60ull) % 1440ull);
        }
        else
        {
            clockMinutes = static_cast<int>((SDL_GetTicksNS() / 1000000ull / 42ull) % 1440ull);
        }

        // Which lighting the frame is under. Indoors the room's own set wins;
        // outdoors, and in any room that did not ship one, this is the zone's.
        const ffxi::Lighting* active = &lighting;
        for (const mh::InteriorLighting& room : interiors)
        {
            if (room.contains(camera.eye()))
            {
                active = &room.lighting;
                break;
            }
        }

        const uint64_t nowTicks = SDL_GetTicksNS();
        const float delta = static_cast<float>(nowTicks - previousTicks) / 1e9f;

        // A trail to back up along, sampled on a timer. Only recorded when the
        // character has actually moved, so standing still does not fill the
        // buffer with the same spot and turn the escape key into a no-op.
        if (character)
        {
            breadcrumbTimer += delta;
            if (breadcrumbTimer >= 0.5f)
            {
                breadcrumbTimer = 0.0f;
                const bool worthKeeping =
                    breadcrumbs.empty() ||
                    std::fabs(breadcrumbs.back().x - characterAt.x) + std::fabs(breadcrumbs.back().z - characterAt.z) >
                        0.4f;
                if (worthKeeping)
                {
                    breadcrumbs.push_back(characterAt);
                    while (breadcrumbs.size() > 32)
                    {
                        breadcrumbs.pop_front();
                    }
                }
            }
        }
        previousTicks = nowTicks;
        // With a line open the keyboard belongs to it. Held keys are read in
        // several places below, so this is cut off at the source rather than
        // guarded at each one - miss a guard and the character walks out from
        // under you mid-sentence.
        static const bool kNoKeysHeld[SDL_SCANCODE_COUNT] = {};
        const bool* held = typing ? kNoKeysHeld : SDL_GetKeyboardState(nullptr);
        // Shift inverts the walk toggle rather than setting it, so holding
        // it walks while running and runs while walking.
        const bool shiftHeld = held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT];
        const bool walking = walkByDefault != shiftHeld;

        // Units a second, for a model 1.79 tall. Twelve read as a sprint and
        // seven still ran a little hot, so the animation and the ground speed
        // agree at these.
        float speed = (walking ? 3.2f : 6.2f) * delta;
        if (held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT])
        {
            // Nothing here: shift means walk now, not sprint.
        }
        // Numpad 8 and 2 move, 4 and 6 turn - which is what they do in the
        // real client, where they are the movement keys rather than a second
        // set of strafes.
        //
        // Nothing moves a corpse. Cut off at the keys rather than around the
        // block that reads them, because that block also settles the body onto
        // the ground, writes the pose the GPU draws and keeps the camera behind
        // the character's shoulder - none of which stops mattering when you
        // die. Skipping the lot left the body frozen wherever it last stood
        // while the camera wandered off on its own.
        const bool backward = !dead && (held[SDL_SCANCODE_S] || held[SDL_SCANCODE_KP_2]);

        // Auto-run ends the moment you ask to go the other way, or die.
        if (backward || dead)
        {
            autoRun = false;
        }
        const bool forward = !dead && (held[SDL_SCANCODE_W] || held[SDL_SCANCODE_KP_8] || autoRun);

        // 4 turns left and 6 turns right, from the character's point of view.
        //
        // Yaw runs the other way here - forward is (sin yaw, 0, cos yaw), so a
        // rising yaw swings to the left - and adding the turn directly made
        // both keys do the opposite of what they say.
        const float turn = (held[SDL_SCANCODE_KP_4] ? 1.0f : 0.0f) - (held[SDL_SCANCODE_KP_6] ? 1.0f : 0.0f);
        if (turn != 0.0f)
        {
            camera.yaw += turn * 2.2f * delta;
        }

        const float ahead = (forward ? speed : 0.0f) - (backward ? speed : 0.0f);
        const float side = dead ? 0.0f
                                : (held[SDL_SCANCODE_D] ? speed : 0.0f) - (held[SDL_SCANCODE_A] ? speed : 0.0f);
        const float lift = (held[SDL_SCANCODE_SPACE] ? speed : 0.0f) - (held[SDL_SCANCODE_LCTRL] ? speed : 0.0f);

        // Numpad 9 and 3 pull the camera in and push it out, the way the real
        // client does it. They used to be space and ctrl, which space now
        // wants for the jump - and the mouse wheel does the same job.
        const float zoom = (held[SDL_SCANCODE_KP_9] ? speed : 0.0f) - (held[SDL_SCANCODE_KP_3] ? speed : 0.0f);

        // Two ways to move: fly the camera around to look at the zone, or
        // drive the character and have the camera follow. A character in the
        // world starts in the second.
        float moved = 0.0f;
        if (driving && character)
        {
            // Movement is relative to where the camera is looking, which is
            // what makes it read as steering a person rather than nudging a
            // point on a map.
            const mh::Vec3 forward = mh::normalise(mh::Vec3{std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)});
            const mh::Vec3 right = mh::normalise(mh::cross(forward, mh::Vec3{0.0f, 1.0f, 0.0f}));

            const mh::Vec3 wanted{characterAt.x + forward.x * ahead + right.x * side, characterAt.y,
                                  characterAt.z + forward.z * ahead + right.z * side};
            if (wanted.x != characterAt.x || wanted.z != characterAt.z)
            {
                const bool ignoreCollision = collision.empty() || noclip;
                const mh::Vec3 stepped = ignoreCollision ? wanted : collision.move(characterAt, wanted, 0.5f);

                // Horizontal only. Whether there is anything to stand on is
                // settled below, by falling - a step off a ledge is a step, not
                // a refusal.
                //
                // The exception is a step onto nothing at all: no surface
                // anywhere below, which means the edge of the world rather
                // than a drop. Falling out of a zone is not a behaviour worth
                // having.
                // Searched over the character's own width rather than under a
                // single point.
                //
                // A staircase is not a continuous surface: between one tread
                // and the next there is a sliver with no walkable triangle
                // over it at all, half a unit wide on the steps outside the
                // Bastok auction house. Asking about one point lands in that
                // gap, reports the edge of the world, and refuses the step -
                // so a perfectly ordinary staircase cannot be climbed or
                // descended. Someone standing with a foot on solid ground is
                // not falling out of the zone.
                const bool intoTheVoid =
                    !ignoreCollision &&
                    !collision.nearestGround(stepped.x, stepped.z, characterAt.y, 1.0f).has_value();

                // What a step is not allowed to do, beyond hitting a wall.
                //
                // Wading is fine, swimming is not - FFXI has none - so water
                // deeper than the knee stops a step the way a wall does.
                //
                // And a drop stops it too. Blocking on walls alone is only as
                // good as the walls: where a zone has a hole in it there is no
                // wall around the hole, so a step off the edge is a legal step
                // into a fall. FFXI does not let a character walk off an edge
                // at all, and neither should this.
                //
                // Measured against the floor under each end rather than
                // against the character's own height, so it reads the same
                // whether they are standing or already in the air.
                //
                // Refused per axis rather than outright, so walking into an
                // edge at an angle slides along it instead of sticking.
                mh::Vec3 allowed = stepped;
                if (!ignoreCollision && !intoTheVoid)
                {
                    const std::optional<float> here =
                        collision.groundAt(characterAt.x, characterAt.z, characterAt.y, kFallReach);

                    // Water is not a barrier. It was one here for a while, on
                    // the reasoning that FFXI has no swimming - but the game
                    // does not stop you walking in, it stops you *getting*
                    // there, and the floor of a canal you have fallen into is
                    // as walkable as any other. Refusing the step modelled the
                    // intent rather than the behaviour, and left a character
                    // stuck at the edge of water it should have been able to
                    // cross. Collision::waterDepthAt still reports the depth;
                    // nothing acts on it.
                    // Climbing out is allowed more headroom than stepping up.
                    // A character wading a canal stands on its floor, and the
                    // quay they walked off is further above them than any kerb
                    // - so with one step height for both, they get in and
                    // cannot get out. FFXI has no swimming, which makes that a
                    // trap rather than a rule.
                    const float stepUp = collision.waterDepthAt(characterAt.x, characterAt.z, characterAt.y)
                                             ? mh::Collision::kWaterStepUp
                                             : mh::Collision::kDefaultStepUp;

                    const auto refused = [&](float x, float z) {
                        const std::optional<float> there =
                            collision.groundAt(x, z, characterAt.y, kFallReach, stepUp);
                        return here && there && (*here - *there) > kMaxStepDown;
                    };

                    if (refused(allowed.x, allowed.z))
                    {
                        allowed = {stepped.x, stepped.y, characterAt.z};
                        if (refused(allowed.x, allowed.z))
                        {
                            allowed = {characterAt.x, stepped.y, stepped.z};
                            if (refused(allowed.x, allowed.z))
                            {
                                allowed = {characterAt.x, stepped.y, characterAt.z};
                            }
                        }
                    }
                }

                if (!intoTheVoid)
                {
                    const float dx = allowed.x - characterAt.x;
                    const float dz = allowed.z - characterAt.z;
                    moved = std::sqrt(dx * dx + dz * dz);
                    characterAt.x = allowed.x;
                    characterAt.z = allowed.z;
                }

                // Facing follows the camera, not the step. Walking backwards or
                // strafing should not spin the character round, and taking the
                // direction from the movement delta means sliding along a wall
                // turns you to face it.
                characterFacing = camera.yaw;
            }

            // Stand on the floor, or fall towards it. Not while noclipping:
            // the point of it is to go through floors as well as walls.
            //
            // groundAt allows a little above the feet - the step height - so
            // walking up a stair tread reads as standing on it rather than as
            // rising through it.
            if (!collision.empty() && !noclip)
            {
                // The same extra headroom the step used. Allowing the step
                // onto a bank but not the rise onto it leaves a character
                // walking at the water's edge without ever getting out.
                const float groundStepUp =
                    collision.waterDepthAt(characterAt.x, characterAt.z, characterAt.y)
                        ? mh::Collision::kWaterStepUp
                        : mh::Collision::kDefaultStepUp;
                const std::optional<float> ground =
                    collision.groundAt(characterAt.x, characterAt.z, characterAt.y, kFallReach, groundStepUp);

                if (ground && *ground >= characterAt.y - kGroundSnap)
                {
                    characterAt.y = *ground;
                    fallSpeed = 0.0f;
                }
                else if (ground)
                {
                    fallSpeed += kGravity * delta;
                    const float next = characterAt.y - fallSpeed * delta;

                    // Land rather than pass through: at speed a single frame
                    // can cover more than the remaining distance.
                    characterAt.y = next <= *ground ? *ground : next;
                    if (next <= *ground)
                    {
                        fallSpeed = 0.0f;
                    }
                }
                else
                {
                    // Nothing underneath at all, which means through the floor
                    // rather than falling: groundAt only looks a step down and
                    // a fall's worth below, so a character who has ended up
                    // under the world finds nothing and simply hangs there,
                    // for ever, watching the zone from below. It happens - a
                    // teleport into a spot with no floor, or a step that got
                    // past the walls - and there was no way back short of
                    // pressing C.
                    //
                    // closestGroundAt looks in both directions and however
                    // far, which is the question this actually wants.
                    if (const std::optional<float> rescue =
                            collision.closestGroundAt(characterAt.x, characterAt.z, characterAt.y))
                    {
                        characterAt.y = *rescue;
                        fallSpeed = 0.0f;
                    }
                }
            }
            writeCharacterInstance();

            // The camera sits behind and above the character, at head height.
            camera.orbiting = true;
            camera.target = {characterAt.x, characterAt.y + 1.2f, characterAt.z};
            // What the player asked for, which a wall may not let them have.
            wantedDistance = std::clamp(wantedDistance - zoom * 0.6f, 1.5f, 25.0f);
            camera.distance = wantedDistance;

            // Indoors the eye ends up through the wall behind the character,
            // looking at the outside of the building they are standing in -
            // which is what logging in inside a house looked like. Pull in to
            // whatever the line of sight hits first.
            //
            // The wanted distance is kept separate from the one in use: pulling
            // in must not be remembered, or stepping back into the open would
            // leave the camera wherever the last doorway put it.
            if (!collision.empty() && !noclip)
            {
                const mh::Vec3 wanted = camera.eye();
                if (auto blocked = collision.firstWallAlong(camera.target, wanted))
                {
                    const float reach = camera.distance * *blocked;
                    camera.distance = std::max(reach - 0.25f, 0.6f);
                }
            }
        }
        else
        {
            camera.walk(ahead, side, lift);
        }

        // Idle, walk or run, chosen by what the character is actually doing
        // - unless a jump is still in the air, which plays to the end.
        const float nowSeconds = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
        // Dead, so nothing else applies.
        //
        // The death clip is not chosen from movement the way idle and walk
        // are: the character is not doing anything, and letting the usual
        // choice run would stand them straight back up the moment the position
        // twitched.
        if (dead)
        {
            const ffxi::Animation* fallen = deadClip ? deadClip : idleClip;
            if (fallen && fallen != playing)
            {
                playing = fallen;
                animationOffset = nowSeconds;
            }
        }
        else if (!pinnedClip && jumpUntil <= nowSeconds)
        {
            // The same walk/run decision the speed uses, so the legs and the
            // ground always agree.
            const ffxi::Animation* moving = walking ? walkClip : runClip;
            if (!moving)
            {
                moving = walking ? runClip : walkClip;   // whichever the model has
            }
            const ffxi::Animation* wanted = moved > 1e-4f ? moving : idleClip;
            if (wanted && wanted != playing)
            {
                playing = wanted;
                animationOffset = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
            }
        }

        // A capture walks the animation a frame at a time; otherwise the clock
        // is either pinned or real.
        // Whatever the client has posted since the last frame. Copied out
        // under the lock rather than read in place, so a long draw never holds
        // up the thread feeding it.
        if (link)
        {
            radarEntities = link->entities();

            // Eased towards where the server says they are.
            //
            // Done here, before anything reads a position, so the instance
            // transforms, the nameplates and the walk/run decision all agree
            // about where an entity is. The rate is per second and framerate
            // independent; a step is caught up in about a tenth of a second,
            // which is short enough not to lag behind a runner and long enough
            // to fill the gaps between updates.
            //
            // A large jump is taken whole rather than glided through: that is
            // a teleport, a zone line or a spawn, and sliding a body across a
            // zone to meet it looks far stranger than the jump it replaces.
            const float sinceLast = lastFrameSeconds > 0.0f
                                        ? std::min(nowSeconds - lastFrameSeconds, 0.25f)
                                        : 0.0f;
            const float ease = 1.0f - std::exp(-12.0f * sinceLast);
            for (mh::RadarEntity& entity : radarEntities)
            {
                auto found = drawnAt.find(entity.id);
                if (found == drawnAt.end())
                {
                    drawnAt.emplace(entity.id, mh::Vec3{entity.x, entity.y, entity.z});
                    continue;
                }

                mh::Vec3& at = found->second;
                const float dx = entity.x - at.x;
                const float dy = entity.y - at.y;
                const float dz = entity.z - at.z;
                if (dx * dx + dy * dy + dz * dz > 64.0f)
                {
                    at = mh::Vec3{entity.x, entity.y, entity.z};
                    continue;
                }

                at.x += dx * ease;
                at.y += dy * ease;
                at.z += dz * ease;
                entity.x = at.x;
                entity.y = at.y;
                entity.z = at.z;
            }

            // Anything that has gone is not worth remembering a position for.
            for (auto it = drawnAt.begin(); it != drawnAt.end();)
            {
                const bool present = std::any_of(radarEntities.begin(), radarEntities.end(),
                                                 [&](const mh::RadarEntity& e) { return e.id == it->first; });
                it = present ? std::next(it) : drawnAt.erase(it);
            }

            lastFrameSeconds = nowSeconds;

            // Nearest first, so the ones that miss out are the ones furthest
            // away.
            //
            // Bodies are drawn from a fixed pool of instance slots and slots
            // were handed out in whatever order the server happened to mention
            // people, so past the cap you did not lose the distant ones - you
            // lost whoever arrived late, which could be the NPC you were
            // standing next to. Sorting here rather than at each use keeps the
            // instance slots, the skinning pass and the nameplates all talking
            // about the same entity for a given index.
            std::sort(radarEntities.begin(), radarEntities.end(),
                      [&](const mh::RadarEntity& a, const mh::RadarEntity& b) {
                          const float ax = a.x - characterAt.x;
                          const float az = a.z - characterAt.z;
                          const float bx = b.x - characterAt.x;
                          const float bz = b.z - characterAt.z;
                          return ax * ax + az * az < bx * bx + bz * bz;
                      });

            // The server moving us, before we report where we think we are -
            // otherwise the position we just posted would win and the move
            // would be undone on the same frame.
            float placedX = 0.0f;
            float placedY = 0.0f;
            float placedZ = 0.0f;
            float placedHeading = 0.0f;
            if (link->takePlacement(placedX, placedY, placedZ, placedHeading))
            {
                characterAt = {placedX, placedY, placedZ};
                characterFacing = placedHeading;

                // The trail is for backing out of somewhere collision has
                // trapped us. Keeping it across a teleport would walk us back
                // to a place that may be in another zone entirely.
                breadcrumbs.clear();
                breadcrumbTimer = 0.0f;

                std::printf("placed at %.1f %.1f %.1f facing %.0f degrees\n", characterAt.x, characterAt.y,
                            characterAt.z, characterFacing * 180.0f / 3.14159265f);
            }

            // Whatever the server last asked for. Checked every frame rather
            // than pushed, because the link is what the session can reach.
            bool musicChanged = false;
            const std::string wantedMusic = link->takeMusic(musicChanged);
            if (musicChanged)
            {
                music.play(wantedMusic);
            }

            link->setCharacter(characterAt.x, characterAt.y, characterAt.z, characterFacing);
            if (link->stopping())
            {
                running = false;
            }
        }

        const float animationSeconds =
            sequenceCount > 0 && playing ? static_cast<float>(std::max(shotIndex, 0)) * playing->frameSeconds()
            : pinnedFrame >= 0.0f && playing
                ? pinnedFrame * playing->frameSeconds()
                : static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f - animationOffset;

        wgpu::SurfaceTexture surfaceTexture;
        surface.GetCurrentTexture(&surfaceTexture);
        if (!surfaceTexture.texture)
        {
            continue;
        }

        const uint32_t width = surfaceTexture.texture.GetWidth();
        const uint32_t height = surfaceTexture.texture.GetHeight();

        wgpu::RenderPassColorAttachment colour{.view = surfaceTexture.texture.CreateView(),
                                               .loadOp = wgpu::LoadOp::Clear,
                                               .storeOp = wgpu::StoreOp::Store,
                                               .clearValue = {0.05, 0.07, 0.09, 1.0}};
        wgpu::RenderPassDepthStencilAttachment depth{.view = depthTexture.CreateView(),
                                                     .depthLoadOp = wgpu::LoadOp::Clear,
                                                     .depthStoreOp = wgpu::StoreOp::Store,
                                                     .depthClearValue = 1.0f};
        wgpu::RenderPassDescriptor passDescriptor{.colorAttachmentCount = 1,
                                                  .colorAttachments = &colour,
                                                  .depthStencilAttachment = &depth};

        // Everyone else's legs.
        //
        // Done before the pass opens because it uploads: each entity is posed
        // on the CPU into its own copy of the model's vertices, and the buffer
        // it draws from is written here rather than mid-pass.
        //
        // Which clip is chosen from how far the entity moved since the last
        // frame - the server sends positions, not gaits, so movement is the
        // only evidence there is. The distance is smoothed because updates
        // arrive a few times a second and a raw per-frame delta is zero on
        // every frame between them, which reads as a stutter rather than a
        // walk.
        int posedCount = 0;
        for (const mh::RadarEntity& entity : radarEntities)
        {
            // Only the ones that will actually be drawn. Skinning is the
            // expensive half - a mesh reposed on the CPU and uploaded every
            // frame - and doing it for a body past the instance cap is work
            // whose result is never submitted.
            if (posedCount >= mh::kMaxDrawnBodies)
            {
                break;
            }

            if (!entity.hasLook() && !entity.hasModel())
            {
                continue;
            }
            ++posedCount;

            const DrawableCharacter* model = modelForEntity(entity);
            if (!model || model->loaded.animations.empty())
            {
                continue;
            }

            AnimatedEntity& state = entityPoses[entity.id];
            if (!state.vertices)
            {
                state.geometry = model->loaded.geometry;
                wgpu::BufferDescriptor descriptor{
                    .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                    .size = state.geometry.vertices.size() * sizeof(mh::Vertex)};
                state.vertices = device.CreateBuffer(&descriptor);
            }

            const float dx = entity.x - state.lastX;
            const float dz = entity.z - state.lastZ;
            const float stepped = state.placed ? std::sqrt(dx * dx + dz * dz) : 0.0f;
            state.lastX = entity.x;
            state.lastZ = entity.z;
            state.placed = true;

            // Speed over the gap between updates, not between frames.
            //
            // Positions arrive a few times a second, so most frames see no
            // movement at all. A per-frame delta is therefore zero on nearly
            // every frame, and anything derived from it crosses the walk and
            // run thresholds several times a second - restarting the clip each
            // time. Dividing the step by the time since the last one gives a
            // speed that does not depend on how often the server speaks, and
            // movingUntil holds the stride across the quiet frames between.
            if (stepped > 1e-4f)
            {
                state.speed = stepped / std::max(nowSeconds - state.lastMoveTime, 1.0f / 60.0f);
                state.lastMoveTime = nowSeconds;
                state.movingUntil = nowSeconds + 0.4f;
            }

            const auto clipNamed = [&model](const char* name) -> const ffxi::Animation* {
                auto found = model->loaded.animations.find(name);
                return found == model->loaded.animations.end() ? nullptr : &found->second;
            };

            const ffxi::Animation* idle = clipNamed("idl0");
            const ffxi::Animation* walk = clipNamed("wlk0");
            const ffxi::Animation* run = clipNamed("run0");

            // Walking in FFXI is a little under three units a second and
            // running a little over five, so the two part company around four.
            const bool moving = nowSeconds < state.movingUntil;
            const ffxi::Animation* wanted = moving ? (state.speed > 4.0f ? run : walk) : idle;
            if (!wanted)
            {
                wanted = idle ? idle : walk;
            }
            if (!wanted)
            {
                continue;
            }

            if (wanted != state.clip)
            {
                state.clip = wanted;
                state.clipStart = nowSeconds;
            }

            // The arms belong to the same stride as the legs: a clip's upper
            // body is its own name with the trailing 0 turned into a 1.
            //
            // No upper half at all when the model has no paired clip, which is
            // what the player does too. Substituting a standing torso for a
            // missing one puts the character in two poses at once - std folds
            // the body forward, so a walking NPC spends the whole stride bowing.
            const ffxi::Animation* upper = nullptr;
            if (state.clip->name.size() == 4 && state.clip->name.back() == '0')
            {
                std::string above = state.clip->name;
                above.back() = '1';
                upper = clipNamed(above.c_str());
            }

            const float elapsed = nowSeconds - state.clipStart;
            const float frame = elapsed / state.clip->frameSeconds();
            const float upperFrame = upper ? elapsed / upper->frameSeconds() : 0.0f;

            mh::reskin(state.geometry,
                       mh::animatedPose(model->loaded.skeleton, *state.clip, frame, upper, upperFrame),
                       model->loaded.meshes);
            queue.WriteBuffer(state.vertices, 0, state.geometry.vertices.data(),
                              state.geometry.vertices.size() * sizeof(mh::Vertex));

            // Where the mesh ended up this frame. The top is where the name
            // goes; the bottom says whether the feet are on the ground, which
            // the rest pose cannot answer once a clip has moved the body.
            float top = state.geometry.vertices.empty() ? 0.0f : state.geometry.vertices[0].position[1];
            for (const mh::Vertex& vertex : state.geometry.vertices)
            {
                top = std::max(top, vertex.position[1]);
            }
            state.posedTop = top;
            state.drawn = true;
        }

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);

        {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const float tanHalfFov = std::tan(1.05f * 0.5f);
            const mh::Vec3 f = camera.orbiting ? mh::normalise(camera.lookAtPoint() - camera.eye()) : camera.forward();
            const mh::Vec3 r = mh::normalise(mh::cross(f, mh::Vec3{0.0f, 1.0f, 0.0f}));
            const mh::Vec3 u = mh::cross(r, f);

            SkyUniforms skyUniforms{};
            skyUniforms.forward[0] = f.x;
            skyUniforms.forward[1] = f.y;
            skyUniforms.forward[2] = f.z;
            skyUniforms.right[0] = r.x * tanHalfFov * aspect;
            skyUniforms.right[1] = r.y * tanHalfFov * aspect;
            skyUniforms.right[2] = r.z * tanHalfFov * aspect;
            skyUniforms.up[0] = u.x * tanHalfFov;
            skyUniforms.up[1] = u.y * tanHalfFov;
            skyUniforms.up[2] = u.z * tanHalfFov;

            const ffxi::LightingSet skySet = active->at(clockMinutes);
            for (size_t i = 0; i < 8; ++i)
            {
                skyUniforms.skyColours[i][0] = skySet.skyColours[i].r;
                skyUniforms.skyColours[i][1] = skySet.skyColours[i].g;
                skyUniforms.skyColours[i][2] = skySet.skyColours[i].b;
                skyUniforms.skyAltitudes[i][0] = skySet.skyAltitudes[i];
            }
            skyUniforms.fogColour[0] = skySet.landscapeFog.r;
            skyUniforms.fogColour[1] = skySet.landscapeFog.g;
            skyUniforms.fogColour[2] = skySet.landscapeFog.b;
            queue.WriteBuffer(skyUniformBuffer, 0, &skyUniforms, sizeof(skyUniforms));

            pass.SetPipeline(skyPipeline);
            pass.SetBindGroup(0, skyBindGroup);
            pass.Draw(3);
        }

        if (indexCount)
        {
            const mh::Mat4 view = mh::lookAt(camera.eye(), camera.lookAtPoint(), mh::Vec3{0, 1, 0});
            // A near plane scaled to the zone puts everything nearby inside it
            // when standing on the ground, so it is fixed rather than relative.
            const mh::Mat4 projection =
                // A far plane 20x the zone radius wastes most of the depth
                // buffer's precision on space nothing occupies, which is what
                // makes coplanar layers fight in the first place.
                mh::perspective(1.05f, static_cast<float>(width) / static_cast<float>(height), 0.25f, radius * 4.0f);

            Uniforms uniforms{};
            const mh::Mat4 viewProjection = projection * view;
            pickProjection = viewProjection;
            havePickProjection = true;
            std::memcpy(uniforms.viewProjection, viewProjection.m, sizeof(uniforms.viewProjection));
            const mh::Vec3 light = mh::normalise(mh::Vec3{0.4f, 0.8f, 0.45f});
            uniforms.lightDirection[0] = light.x;
            uniforms.lightDirection[1] = light.y;
            uniforms.lightDirection[2] = light.z;

            const ffxi::LightingSet set = active->at(clockMinutes);
            uniforms.ambient[0] = set.landscapeAmbient.r * 0.5f;
            uniforms.ambient[1] = set.landscapeAmbient.g * 0.5f;
            uniforms.ambient[2] = set.landscapeAmbient.b * 0.5f;
            uniforms.sunlight[0] = set.landscapeSunlight.r * 0.5f;
            uniforms.sunlight[1] = set.landscapeSunlight.g * 0.5f;
            uniforms.sunlight[2] = set.landscapeSunlight.b * 0.5f;
            uniforms.fogColour[0] = set.landscapeFog.r;
            uniforms.fogColour[1] = set.landscapeFog.g;
            uniforms.fogColour[2] = set.landscapeFog.b;
            uniforms.fogRange[0] = set.landscapeMinFog;
            uniforms.fogRange[1] = set.landscapeMaxFog > 0.0f ? set.landscapeMaxFog : 10000.0f;
            const mh::Vec3 eyePoint = camera.eye();
            uniforms.eye[0] = eyePoint.x;
            uniforms.eye[1] = eyePoint.y;
            uniforms.eye[2] = eyePoint.z;
            // Seconds since start, for the water surface.
            uniforms.eye[3] = animationSeconds;

            // With no lighting data, fall back to a plain lit look rather than
            // a black zone.
            if (lighting.empty())
            {
                uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = 0.35f;
                uniforms.sunlight[0] = uniforms.sunlight[1] = uniforms.sunlight[2] = 0.65f;
                uniforms.fogRange[1] = 1e9f;
            }
            // shaderMode: 0 draws colour with no alpha discard,
            // 2 draws alpha as greyscale, unset is normal rendering.
            uniforms.lightDirection[3] = shaderMode;
            queue.WriteBuffer(uniformBuffer, 0, &uniforms, sizeof(uniforms));

            pass.SetVertexBuffer(0, vertexBuffer);
            pass.SetVertexBuffer(1, instanceBuffer);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            // Two passes over the same list: everything solid, then the water
            // over the top of it. Water blends and does not write depth, so it
            // has to come after whatever is meant to show through it, and a
            // list in model-name order does not give that for free.
            for (int layer = 0; layer < 2; ++layer)
            {
                for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
                {
                    const mh::InstancedDraw& draw = zone->draws[i];
                    const bool translucent = draw.water || draw.blend;
                    if (translucent != (layer == 1))
                    {
                        continue;
                    }
                    // cutoutMode: 0 never cuts out, 1 always, otherwise the
                    // texture's own measurement decides.
                    const bool cutout = cutoutMode == 0   ? false
                                        : cutoutMode == 1 ? true
                                                          : draw.cutout;
                    pass.SetPipeline(translucent ? translucentPipeline : (cutout ? cutoutPipeline : pipeline));
                    pass.SetBindGroup(0, batchBindGroups[i]);
                    pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
                }
            }

            if (!characterBindGroups.empty())
            {
                if (playing)
                {
                    float frame = animationSeconds / playing->frameSeconds();

                    // Falling over happens once. Every clip is sampled with a
                    // wrap - see animatedPose - which is right for a walk and
                    // wrong for a death: left to loop, the body collapsed,
                    // snapped upright and collapsed again for as long as you
                    // were dead. Held on the last frame instead, so a corpse
                    // stays a corpse.
                    if (playing == deadClip && deadClip && deadClip->frames > 0)
                    {
                        frame = std::min(frame, static_cast<float>(deadClip->frames) - 1.0f);
                    }
                    // The arms belong to the same stride as the legs, so
                    // the upper body is stepped on its own frame rate but off
                    // the same clock.
                    const ffxi::Animation* upperClip = upperFor ? upperFor(playing) : nullptr;
                    const float upperFrame =
                        upperClip ? animationSeconds / upperClip->frameSeconds() : 0.0f;
                    mh::reskin(character->geometry,
                               mh::animatedPose(character->skeleton, *playing, frame, upperClip, upperFrame),
                               character->meshes);
                    queue.WriteBuffer(characterVertexBuffer, 0, character->geometry.vertices.data(),
                                      character->geometry.vertices.size() * sizeof(mh::Vertex));
                }
                pass.SetVertexBuffer(1, characterInstanceBuffer);
                pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);

                // Instance 0 is the player, animated. The rest are everyone
                // else, from the still buffer, reached with firstInstance so
                // they read the same instance data without sharing a pose.
                for (size_t i = 0; i < character->geometry.batches.size() && i < characterBindGroups.size(); ++i)
                {
                    const mh::Batch& batch = character->geometry.batches[i];
                    pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                    pass.SetBindGroup(0, characterBindGroups[i]);

                    pass.SetVertexBuffer(0, characterVertexBuffer);
                    pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0, 0);

                    // Everyone without a model of their own, from the shared
                    // body. Drawn one at a time rather than as a run because
                    // the entities that do have models are interleaved with
                    // them, and instance slots are assigned by position in the
                    // list rather than by which model is used.
                    for (int body = 0; body < drawnBodies && entityVertexBuffer; ++body)
                    {
                        const size_t index = static_cast<size_t>(body);
                        // Only skipped if something actually built. A
                        // Chocobo's look names a race the player race table has
                        // no entry for, and dropping it would leave nothing
                        // standing there at all.
                        //
                        // This has to ask the same question the model loop
                        // asks, creatures included: testing only the equipment
                        // form drew the shared body underneath every monster,
                        // so a rabbit came with a Tarutaru standing inside it.
                        if (index < radarEntities.size() && modelForEntity(radarEntities[index]))
                        {
                            continue;   // has its own model, drawn below
                        }

                        pass.SetVertexBuffer(0, entityVertexBuffer);
                        pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0,
                                         static_cast<uint32_t>(body + 1));
                    }
                }

                // And everyone the server described well enough to build: their
                // own race, their own clothes. One model per distinct look,
                // shared by everybody wearing it.
                for (int body = 0; body < drawnBodies; ++body)
                {
                    const size_t index = static_cast<size_t>(body);
                    if (index >= radarEntities.size() ||
                        (!radarEntities[index].hasLook() && !radarEntities[index].hasModel()))
                    {
                        continue;
                    }

                    const DrawableCharacter* model = modelForEntity(radarEntities[index]);
                    if (!model)
                    {
                        continue;
                    }

                    // The entity's own posed vertices when it has them, and
                    // the shared model's when it does not - a look with no
                    // animations at all is still worth drawing standing still.
                    auto posed = entityPoses.find(radarEntities[index].id);
                    const bool animated = posed != entityPoses.end() && posed->second.drawn;
                    pass.SetVertexBuffer(0, animated ? posed->second.vertices : model->vertices);
                    pass.SetIndexBuffer(model->indices, wgpu::IndexFormat::Uint32);
                    for (size_t b = 0; b < model->loaded.geometry.batches.size() && b < model->bindGroups.size(); ++b)
                    {
                        const mh::Batch& batch = model->loaded.geometry.batches[b];
                        pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                        pass.SetBindGroup(0, model->bindGroups[b]);
                        pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0,
                                         static_cast<uint32_t>(body + 1));
                    }
                }

                // The player's index buffer again, for whatever draws next.
                pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);
            }

            // The zone's exits, standing in the world.
            //
            // Drawn after the world and the characters so the glow lies over
            // them, and before the HUD so it stays behind the panels.
            if (zoneLinePipeline && zoneLineBindGroup)
            {
                const std::vector<mh::ZoneLineMarker> lines = link ? link->zoneLines()
                                                                   : std::vector<mh::ZoneLineMarker>{};
                int drawn = std::min(static_cast<int>(lines.size()), mh::kZoneLineMarkers);

                // The target gets a ring of its own, after the zone lines. FFXI draws
                // one under whatever you have selected, and the pipeline that draws a
                // glowing ring at a world position already exists.
                int targetRing = -1;
                mh::Vec3 targetAt{};
                if (targetId != 0 && drawn < mh::kZoneLineMarkers)
                {
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        if (entity.id == targetId)
                        {
                            targetAt = mh::Vec3{entity.x, entity.y, entity.z};
                            targetRing = drawn++;
                            break;
                        }
                    }
                }
                if (drawn > 0)
                {
                    ZoneLineUniforms markers{};
                    std::memcpy(markers.viewProjection, viewProjection.m, sizeof(markers.viewProjection));
                    markers.counts[0] = static_cast<float>(drawn);
                    markers.counts[1] = mh::kZoneLineHeight;
                    markers.counts[2] = nowSeconds;
                    markers.counts[3] = static_cast<float>(targetRing);

                    for (int i = 0; i < drawn; ++i)
                    {
                        if (i == targetRing)
                        {
                            // Tight enough to read as standing around one
                            // person rather than marking a place.
                            markers.lines[i][0] = targetAt.x;
                            markers.lines[i][1] = targetAt.y;
                            markers.lines[i][2] = targetAt.z;
                            markers.lines[i][3] = 0.7f;
                            continue;
                        }

                        markers.lines[i][0] = lines[static_cast<size_t>(i)].x;
                        markers.lines[i][1] = lines[static_cast<size_t>(i)].y;
                        markers.lines[i][2] = lines[static_cast<size_t>(i)].z;
                        markers.lines[i][3] = lines[static_cast<size_t>(i)].radius;
                    }

                    queue.WriteBuffer(zoneLineUniformBuffer, 0, &markers, sizeof(markers));
                    pass.SetPipeline(zoneLinePipeline);
                    pass.SetBindGroup(0, zoneLineBindGroup);
                    pass.Draw(mh::kZoneLineSegments * 6, static_cast<uint32_t>(drawn));
                }
            }

            // Water goes here: after the world it sits in, and before
            // everything drawn on top of the world.
            //
            // It was last, which put a translucent sheet over the radar, the
            // clock and the nameplates - the HUD is drawn in the same pass and
            // does not write depth, so whatever comes after it simply wins. A
            // canal at the edge of the screen tinted the compass.
            if (waterIndexCount && waterPipeline)
            {
                pass.SetPipeline(waterPipeline);
                pass.SetBindGroup(0, waterBindGroup);
                pass.SetVertexBuffer(0, waterVertexBuffer);
                pass.SetIndexBuffer(waterIndexBuffer, wgpu::IndexFormat::Uint32);
                pass.DrawIndexed(waterIndexCount);
            }

            if (radarPipeline && radarBindGroup)
            {
                RadarUniforms radar{};
                // Top right, a fifth of the shorter side across, and low
                // enough to leave room for the clock over it - at 0.70 the
                // radar's own top edge was already at 0.96 and the clock drew
                // off the top of the window.
                radar.placement[0] = 0.78f;
                radar.placement[1] = 0.50f;
                radar.placement[2] = 0.21f;
                radar.placement[3] = static_cast<float>(width) / static_cast<float>(height);

                radar.mapExtent[0] = mapCentreX;
                radar.mapExtent[1] = mapCentreZ;
                radar.mapExtent[2] = mapHalf;

                // The character if there is one, otherwise wherever the camera
                // is - a radar centred on nothing is worse than no radar.
                const mh::Vec3 eyePosition = camera.eye();
                radar.viewer[0] = character ? characterAt.x : eyePosition.x;
                radar.viewer[1] = character ? characterAt.z : eyePosition.z;
                radar.viewer[2] = character ? characterFacing : camera.yaw;
                radar.viewer[3] = radarRange;

                const size_t shown = std::min(radarEntities.size(), static_cast<size_t>(mh::kRadarMaxEntities));
                radar.counts[0] = static_cast<float>(shown);
                radar.counts[2] = radarTurns ? 1.0f : 0.0f;
                // The radar's own zone label is off. It was drawn from the
                // 4x6 bitmap font across the bottom of the map, and the HUD
                // now sets the same name in a real typeface underneath - two
                // of them, one of them worse, in the same place.
                radar.counts[1] = 0.0f;
                for (size_t i = 0; i < shown; ++i)
                {
                    radar.entities[i][0] = radarEntities[i].x;
                    radar.entities[i][1] = radarEntities[i].z;
                    radar.entities[i][2] = static_cast<float>(radarEntities[i].kind);
                }
                queue.WriteBuffer(radarUniformBuffer, 0, &radar, sizeof(radar));

                pass.SetPipeline(radarPipeline);
                pass.SetBindGroup(0, radarBindGroup);
                pass.Draw(3);
            }

            if (hudPipeline && hudBindGroup)
            {
                HudUniforms hud{};
                const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
                // One atlas texel to one screen pixel.
                //
                // Raising this by hand twice did not fix the text, because the
                // problem was never the size - it was the sampling. The atlas
                // was generated at a 72 pixel cell and drawn at about 19, so
                // every glyph was minified nearly four times through a linear
                // filter with no mipmaps, which thins the strokes and softens
                // the edges into mush. Bigger only made bigger mush.
                //
                // Deriving the size from the atlas and the window instead
                // keeps it at 1:1, where the glyph the generator drew is the
                // glyph that reaches the screen. The atlas is generated near
                // display size for the same reason; a scale far from 1.0 will
                // soften again, so the labels stay close to it.
                hud.counts[1] = static_cast<float>(textFont.cell) * 2.0f / static_cast<float>(height);
                hud.counts[2] = windowAspect;
                hud.atlas[0] = static_cast<float>(textFont.columns);
                hud.atlas[1] = static_cast<float>(textFont.cell);
                hud.atlas[2] = static_cast<float>(textFont.width);
                hud.atlas[3] = static_cast<float>(textFont.height);

                int labels = 0;

                // Laid out around the radar rather than at fixed corners, so
                // the two stay together if the radar ever moves.
                const float radarCentreX = 0.78f;
                const float radarCentreY = 0.50f;
                const float radarRadius = 0.21f;

                // Stacked by the height of a line rather than by numbers
                // chosen to look right at one text size. The size is derived
                // from the atlas and the window now, so anything hand-tuned
                // against it comes apart the moment either changes - which is
                // exactly what happened when the text went to 1:1 and every
                // label landed on its neighbour.
                const float line = hud.counts[1];
                const float gap = line * 0.18f;
                const float above = radarCentreY + radarRadius;

                // Every label's size in one place. The compass is set where it
                // is drawn, because the letters also have to be centred on
                // their points.
                constexpr float kZoneScale = 0.4f;
                constexpr float kPositionScale = 0.4f;
                constexpr float kWeekdayScale = 0.4f;
                const float below = radarCentreY - radarRadius;

                // centred = the x given is the middle, otherwise it is the
                // left edge. Chat reads down a column and has to line up.
                // How wide some text will be once placed. The same sum of
                // advances place() walks, so a box sized from this and the text
                // drawn into it cannot disagree.
                const auto measure = [&](const std::string& text, float scale) {
                    float pen = 0.0f;
                    const float cellSize = static_cast<float>(textFont.cell);
                    for (char raw : text)
                    {
                        pen += textFont.advanceOf(raw) / cellSize;
                    }
                    return pen * ((hud.counts[1] * scale) / windowAspect);
                };

                const auto place = [&](const std::string& text, float x, float bottomY, float scale,
                                       const float* tint, float background, bool centred) {
                    if (labels >= mh::kHudStrings || text.empty() || textFont.empty())
                    {
                        return;
                    }
                    const float cellSize = static_cast<float>(textFont.cell);
                    float pen = 0.0f;
                    int written = 0;
                    for (char raw : text)
                    {
                        if (written >= mh::kHudChars)
                        {
                            break;
                        }
                        const float advance = textFont.advanceOf(raw) / cellSize;
                        float* glyph = hud.glyphs[labels * mh::kHudChars + written];
                        glyph[0] = static_cast<float>(textFont.indexOf(raw));
                        glyph[1] = pen;
                        glyph[2] = advance;
                        pen += advance;
                        ++written;
                    }

                    // Centred on the radar's column, which is what makes the
                    // clock and the zone name read as one instrument.
                    const float cellWide = (hud.counts[1] * scale) / windowAspect;
                    hud.boxes[labels][0] = centred ? x - pen * cellWide * 0.5f : x;
                    hud.boxes[labels][1] = bottomY;
                    hud.boxes[labels][2] = pen;
                    hud.boxes[labels][3] = background;
                    hud.colours[labels][0] = tint[0];
                    hud.colours[labels][1] = tint[1];
                    hud.colours[labels][2] = tint[2];
                    hud.colours[labels][3] = scale;
                    ++labels;
                };

                // Wrapped so the existing calls keep reading as they did.
                const auto label = [&](const std::string& text, float centreX, float bottomY, float scale,
                                       const float* tint, float background) {
                    place(text, centreX, bottomY, scale, tint, background, true);
                };

                // The clock, above the radar. Vana'diel's week is eight days
                // and its hour is 2.4 real seconds; both come from the
                // server's own clock when it gave us one.
                static const char* kWeekdays[8] = {"Firesday",   "Earthsday",    "Watersday", "Windsday",
                                                   "Iceday",     "Lightningday", "Lightsday", "Darksday"};
                char clock[32] = {};
                std::snprintf(clock, sizeof(clock), "%02d:%02d", clockMinutes / 60, clockMinutes % 60);
                const float clockBottom = above + gap * 2.0f + line * 1.5f;
                label(clock, radarCentreX, clockBottom, 0.85f, kHudBright, 0.55f);

                if (vanaSeconds > 0)
                {
                    label(kWeekdays[(vanaSeconds / 86400ull) % 8ull], radarCentreX,
                          clockBottom - line * kWeekdayScale - gap * 0.5f, kWeekdayScale, kHudDim, 0.55f);
                }

                // Compass letters around the ring.
                //
                // The map itself is north up - the radar maps its offsets
                // straight to world coordinates without rotating them - so
                // these belong at fixed points. The part that moves is the
                // heading notch the radar already draws; a ring that turned as
                // well would disagree with the map under it.
                float southY = 0.0f;
                {
                    const float ringX = radarRadius * 1.16f / windowAspect;
                    const float ringY = radarRadius * 1.16f;
                    const float compass = 0.5f;
                    const float half = hud.counts[1] * compass * 0.5f;

                    // The letters ride the ring rather than sitting at fixed
                    // corners, because with the map turning they have to. A
                    // compass that says north is up while the map has turned
                    // under it is worse than no compass.
                    //
                    // Screen direction for a world bearing is the inverse of
                    // the turn the map made, so north goes to (-sin, cos).
                    const float turn = radarTurns ? characterFacing : 0.0f;
                    const float turnCos = std::cos(turn);
                    const float turnSin = std::sin(turn);
                    const auto onRing = [&](const char* letter, float worldX, float worldY, const float* tint) {
                        const float screenX = worldX * turnCos - worldY * turnSin;
                        const float screenY = worldX * turnSin + worldY * turnCos;
                        label(letter, radarCentreX + screenX * ringX, radarCentreY + screenY * ringY - half,
                              compass, tint, 0.0f);
                    };

                    // North in red, the way the game does it. With the map
                    // turning, north is somewhere different every time you look
                    // at it, and one letter that stands out is faster to find
                    // than four that read the same.
                    static constexpr float kNorthRed[3] = {1.00f, 0.32f, 0.28f};
                    onRing("N", 0.0f, 1.0f, kNorthRed);
                    onRing("S", 0.0f, -1.0f, kHudDim);
                    onRing("E", 1.0f, 0.0f, kHudDim);
                    onRing("W", -1.0f, 0.0f, kHudDim);

                    // The zone name still hangs below the circle, so it needs
                    // somewhere fixed regardless of where south ended up.
                    southY = radarCentreY - ringY - half;
                }

                // Somewhere to send a bug from, top left, without leaving
                // the world.
                //
                // The launcher has the same two links, but the launcher hides
                // itself while the world is up - which is exactly when a
                // player finds something worth reporting.
                //
                // Measured as they are drawn: the text is proportional and the
                // chip is sized to fit it, so working out where one is means
                // doing most of the work of drawing it. The same reason the
                // death box does this.
                {
                    constexpr float kCornerScale = 0.7f;
                    const float chipHeight = line * kCornerScale + gap * 0.6f;
                    float cornerX = -0.97f;
                    const float cornerY = 0.93f;

                    const char* names[2] = {"Discord", "Report a bug"};
                    const mh::ViewerLink::Link targets[2] = {mh::ViewerLink::Link::Discord,
                                                             mh::ViewerLink::Link::Issues};
                    for (int i = 0; i < 2; ++i)
                    {
                        const float width = measure(names[i], kCornerScale);
                        place(names[i], cornerX, cornerY, kCornerScale, kHudDim, 0.55f, false);

                        cornerLinks[i].left = cornerX;
                        cornerLinks[i].bottom = cornerY;
                        cornerLinks[i].width = width;
                        cornerLinks[i].height = chipHeight;
                        cornerLinks[i].target = targets[i];

                        cornerX += width + gap * 2.0f;
                    }
                }

                // HP, MP and TP, bottom left, above the chat log.
                //
                // The one thing the window never said was whether the player
                // was alive. Being dead read as being unable to move, which is
                // indistinguishable from a stuck client, and that is exactly
                // how it was reported. The numbers come straight off
                // GP_SERV_COMMAND_GROUP_ATTR; the percentages are the server's
                // own, so a full bar means what the server thinks it means.
                if (link)
                {
                    const mh::ViewerLink::Vitals vitals = link->vitals();
                    if (vitals.known)
                    {
                        constexpr float kVitalScale = 0.75f;
                        const float vitalLeft = -0.97f;
                        float vitalY = -0.62f;

                        // Red when it is low enough to matter, and plainly
                        // different when it is zero - a corpse should not look
                        // like a character on one hit point.
                        const float kDead[3] = {1.00f, 0.35f, 0.35f};
                        const float kHurt[3] = {1.00f, 0.78f, 0.42f};
                        const bool dead = vitals.hp == 0;
                        const float* hpTint = dead ? kDead : (vitals.hpPercent <= 25 ? kHurt : kHudBright);

                        char row[48] = {};
                        std::snprintf(row, sizeof(row), "HP %u  (%u%%)", vitals.hp, vitals.hpPercent);
                        place(row, vitalLeft, vitalY, kVitalScale, hpTint, 0.55f, false);
                        vitalY -= line * kVitalScale + gap * 0.4f;

                        std::snprintf(row, sizeof(row), "MP %u  (%u%%)", vitals.mp, vitals.mpPercent);
                        place(row, vitalLeft, vitalY, kVitalScale, kHudDim, 0.55f, false);
                        vitalY -= line * kVitalScale + gap * 0.4f;

                        std::snprintf(row, sizeof(row), "TP %u", vitals.tp);
                        place(row, vitalLeft, vitalY, kVitalScale, kHudDim, 0.55f, false);

                        if (dead)
                        {
                            vitalY -= line * kVitalScale + gap * 0.4f;
                            place("DEAD", vitalLeft, vitalY, kVitalScale, kDead, 0.55f, false);
                        }
                    }
                }

                // The zone name, as a ribbon under the radar, with the position
                // directly beneath it - the two are one block, and splitting
                // them either side of the compass read as two unrelated
                // things.
                const float zoneNameBottom = southY + line * 1.15f;
                if (options.zoneName)
                {
                    // Underscores are how the zone tables spell a space, and
                    // nobody wants to read Bastok_Markets.
                    std::string zone = *options.zoneName;
                    for (char& letter : zone)
                    {
                        if (letter == '_')
                        {
                            letter = ' ';
                        }
                    }
                    label(zone, radarCentreX, zoneNameBottom, kZoneScale, kHudBright, 0.55f);
                }

                // And where we are. FFXI shows a lettered grid here; that
                // comes from map bounds in the DATs which nothing reads yet,
                // so these are the coordinates the server itself uses - which
                // at least match what !pos prints.
                if (character)
                {
                    char position[32] = {};
                    std::snprintf(position, sizeof(position), "%.0f  %.0f", characterAt.x, -characterAt.z);
                    label(position, radarCentreX, zoneNameBottom - line * kPositionScale - gap * 0.5f,
                          kPositionScale, kHudDim, 0.55f);
                }

                // Chat, bottom left, oldest at the top. Same atlas as
                // everything else: this was the last thing still drawing from
                // the 4x6 bitmap font, which had no lower case at all.
                {
                    std::vector<std::string> lines = link ? link->chat() : options.testChat;
                    if (lines.empty())
                    {
                        lines.push_back("Chat - waiting for the server");
                    }
                    const float line = hud.counts[1] * 0.4f;

                    // The panel stacks upwards from the bottom, so the line
                    // being typed takes the bottom row and the history moves up
                    // to make room rather than being written over.
                    const float base = typing ? -0.97f + line * 1.15f : -0.97f;
                    for (size_t i = 0; i < lines.size(); ++i)
                    {
                        const float bottom = base + line * 1.15f * static_cast<float>(lines.size() - 1 - i);
                        place(lines[i], -0.98f, bottom, 0.4f, kHudBright, 0.5f, false);
                    }

                    if (typing)
                    {
                        place("> " + typed + "_", -0.98f, -0.97f, 0.4f, kHudBright, 0.65f, false);
                    }
                }

                if (labels > 0)
                {
                    hud.counts[0] = static_cast<float>(labels);
                    queue.WriteBuffer(hudUniformBuffer, 0, &hud, sizeof(hud));
                    pass.SetPipeline(hudPipeline);
                    pass.SetBindGroup(0, hudBindGroup);
                    pass.Draw(3);
                }
            }

            if (platePipeline && plateBindGroup && !radarEntities.empty())
            {
                NameplateUniforms plate{};
                std::memcpy(plate.viewProjection, viewProjection.m, sizeof(plate.viewProjection));

                const float windowAspect = static_cast<float>(width) / static_cast<float>(height);
                // One atlas cell, in NDC y. A cell is taller than the glyph
                // inside it - the outline needs somewhere to go - so the text
                // reads a good deal smaller than this number suggests.
                // 1:1 with the atlas, as the HUD is - see there. Names sit
                // at a distance and shrink with it, so this is the size they
                // reach at their crispest rather than a size they always are.
                // Smaller than 1:1 with the atlas. Names sit out in the world
                // rather than pinned to a panel, and a dozen of them at full
                // size cover more of the zone than they label.
                plate.counts[1] = static_cast<float>(textFont.cell) * 2.0f * 0.6f / static_cast<float>(height);
                plate.counts[2] = windowAspect;
                plate.atlas[0] = static_cast<float>(textFont.columns);
                plate.atlas[1] = static_cast<float>(textFont.cell);
                plate.atlas[2] = static_cast<float>(textFont.width);
                plate.atlas[3] = static_cast<float>(textFont.height);

                // The zone's own name table, loaded the first time an entity
                // arrives. The server sends NPCs with no name, and the numeric
                // zone is not otherwise known here - but every entity id
                // carries the zone it belongs to, so the first one to arrive
                // says which table to read.
                if (!triedEntityNames && !radarEntities.empty())
                {
                    for (const mh::RadarEntity& entity : radarEntities)
                    {
                        if (entity.id > 0x1000000u)
                        {
                            const auto zone = static_cast<uint16_t>((entity.id - 0x1000000u) >> 12);
                            try
                            {
                                const ffxi::FileTable table{ffxi::defaultInstallRoot()};
                                entityNames = ffxi::EntityNames::load(table, zone);
                            }
                            catch (const std::exception& e)
                            {
                                std::printf("no entity names: %s\n", e.what());
                            }
                            triedEntityNames = true;
                            break;
                        }
                    }
                }

                // Laid out here rather than in the shader: the font is
                // proportional, so where a glyph starts depends on every glyph
                // before it, and a fragment shader cannot accumulate that per
                // pixel without redoing the whole string. Measured in cells, so
                // the shader scales one number to whatever size it draws at.
                const auto layOutPlate = [&textFont](NameplateUniforms& into, int slot, const std::string& text) {
                    const float cell = static_cast<float>(textFont.cell);
                    float pen = 0.0f;
                    int written = 0;
                    for (char raw : text)
                    {
                        if (written >= mh::kNameplateChars)
                        {
                            break;
                        }
                        const float advance = textFont.advanceOf(raw) / cell;
                        float* glyph = into.glyphs[slot * mh::kNameplateChars + written];
                        glyph[0] = static_cast<float>(textFont.indexOf(raw));
                        glyph[1] = pen;
                        glyph[2] = advance;
                        pen += advance;
                        ++written;
                    }
                    for (int spare = written; spare < mh::kNameplateChars; ++spare)
                    {
                        into.glyphs[slot * mh::kNameplateChars + spare][2] = 0.0f;
                    }
                    into.positions[slot][3] = pen;   // total width, in cells
                    return slot + 1;
                };

                int named = 0;

                // Our own name, over our own head. The game shows everyone
                // their own nameplate; leaving it off made our character the
                // only anonymous one on screen.
                if (options.playerName && !options.playerName->empty() && character)
                {
                    plate.positions[named][0] = characterAt.x;
                    plate.positions[named][1] = characterAt.y + character->geometry.height() + kPlateClearance;
                    plate.positions[named][2] = characterAt.z;
                    plate.colours[named][0] = kNameWhite[0];
                    plate.colours[named][1] = kNameWhite[1];
                    plate.colours[named][2] = kNameWhite[2];
                    named = layOutPlate(plate, named, *options.playerName);
                }

                for (const mh::RadarEntity& entity : radarEntities)
                {
                    if (named >= mh::kNameplateMax)
                    {
                        break;
                    }

                    // The server's name where it gave one - players - and the
                    // zone's table otherwise, which is where every NPC's name
                    // actually lives.
                    // Doors, zone lines and scenery are named in the zone's
                    // table, and the game shows the name only on target.
                    // Drawing them all labels a city with things nobody asked
                    // about.
                    if (entity.nameHidden)
                    {
                        continue;
                    }

                    // Nothing to label. warp07 in Windurst Waters is model 50,
                    // and that file holds one black triangle and nothing else -
                    // a trigger the game never draws - so a plate over it names
                    // bare ground. The zone table has a name for it either way,
                    // because these are the client's own internal names.
                    if (!modelForEntity(entity))
                    {
                        continue;
                    }

                    const std::string& shown =
                        entity.name.empty() ? entityNames.lookup(entity.id) : entity.name;
                    if (shown.empty())
                    {
                        continue;
                    }

                    // Over the head rather than at the feet. The model is about
                    // 1.8 tall and the name wants a little air above that.
                    // Each entity's own model decides how high its name sits.
                    // A galka and a tarutaru standing together want quite
                    // different numbers, and the shared body is the fallback
                    // for anyone we could not build.
                    // The pose this frame if there is one, because a clip can
                    // carry the body well away from where the rest pose put it.
                    const DrawableCharacter* plateModel = modelForEntity(entity);
                    auto posedPlate = entityPoses.find(entity.id);
                    const float bodyHeight =
                        posedPlate != entityPoses.end() && posedPlate->second.drawn && posedPlate->second.posedTop > 0.0f
                            ? posedPlate->second.posedTop
                            : (plateModel ? plateModel->loaded.geometry.height()
                                          : (character ? character->geometry.height() : 1.8f));

                    const float headY = entity.y + bodyHeight + kPlateClearance;

                    // Behind a wall, so not shown.
                    //
                    // The depth test cannot do this: the plates are drawn as one
                    // fullscreen triangle at depth zero with the projection done
                    // per fragment, so every label passes any depth comparison
                    // there is. The same wall raycast the camera uses to avoid
                    // orbiting through a house answers it directly instead, and
                    // a handful of entities is a handful of rays.
                    if (collision.firstSolidAlong(camera.eye(), mh::Vec3{entity.x, headY, entity.z}))
                    {
                        continue;
                    }

                    plate.positions[named][0] = entity.x;
                    plate.positions[named][1] = headY;
                    plate.positions[named][2] = entity.z;

                    // Colour by what the entity is. The rest of the palette -
                    // party blue, linkshell green, friend orange, pet grey -
                    // needs membership the client does not parse yet, so those
                    // stay white rather than being guessed at.
                    const float* tint = kNameWhite;
                    if (entity.gmLevel > 0)
                    {
                        // Ahead of everything else: a GM is a GM whatever else
                        // they are, and the real client says so first too.
                        tint = kNameGm;
                    }
                    else if (entity.kind == 2)
                    {
                        // A mob the server says has no health left. The bit
                        // that marks a mob is literally `hp > 0`, so a corpse
                        // stops announcing itself as one - the tracker keeps
                        // it an enemy anyway, and this is what says it is over.
                        tint = entity.healthPercent == 0 ? kNameDead : kNameMonster;
                    }
                    else if (entity.kind == 1)
                    {
                        tint = kNameNpc;
                    }
                    plate.colours[named][0] = tint[0];
                    plate.colours[named][1] = tint[1];
                    plate.colours[named][2] = tint[2];

                    named = layOutPlate(plate, named, shown);
                }

                if (named > 0)
                {
                    plate.counts[0] = static_cast<float>(named);
                    queue.WriteBuffer(plateUniformBuffer, 0, &plate, sizeof(plate));
                    pass.SetPipeline(platePipeline);
                    pass.SetBindGroup(0, plateBindGroup);
                    pass.Draw(3);
                }
            }

        }

        // The box a dead character gets, drawn over everything else.
        //
        // Outside the geometry check above on purpose: whether the zone drew
        // has nothing to do with whether the player is lying on the floor of
        // it, and a box that appeared only in zones we could read would go
        // missing exactly where the client is already least useful.
        //
        // `dead` and `raiseOffered` were read at the top of the frame, with
        // the movement gate and the pose, so what the box says and what the
        // body does cannot disagree.
        deathBoxShown = false;
        for (DialogButton& button : deathButtons)
        {
            button = DialogButton{};
        }

        if (dead && dialogPipeline && dialogBindGroup && !textFont.empty())
        {
            DialogUniforms box{};
            const float windowAspect = static_cast<float>(width) / static_cast<float>(height);

            // 1:1 with the atlas, as the HUD is - see there for why the size
            // is derived from the window rather than chosen.
            box.counts[1] = static_cast<float>(textFont.cell) * 2.0f / static_cast<float>(height);
            box.counts[2] = windowAspect;
            box.counts[3] = 0.42f;
            box.atlas[0] = static_cast<float>(textFont.columns);
            box.atlas[1] = static_cast<float>(textFont.cell);
            box.atlas[2] = static_cast<float>(textFont.width);
            box.atlas[3] = static_cast<float>(textFont.height);

            const float line = box.counts[1];

            // One row's glyphs, and how wide they came to in cells. The same
            // shape as layOutPlate above and there for the same reason: the
            // font is proportional, so only the CPU knows where a letter
            // starts.
            const auto layOutRow = [&textFont](DialogUniforms& into, int row, const std::string& text)
            {
                const float cell = static_cast<float>(textFont.cell);
                float pen = 0.0f;
                int written = 0;
                for (char raw : text)
                {
                    if (written >= mh::kDialogChars)
                    {
                        break;
                    }
                    const float advance = textFont.advanceOf(raw) / cell;
                    float* glyph = into.glyphs[row * mh::kDialogChars + written];
                    glyph[0] = static_cast<float>(textFont.indexOf(raw));
                    glyph[1] = pen;
                    glyph[2] = advance;
                    pen += advance;
                    ++written;
                }
                into.boxes[row][2] = pen;   // total width, in cells
                return pen;
            };

            // What it says. The middle line changes the moment a raise is
            // offered, because that is the whole of why anyone waits.
            const char* said[3] = {
                "You have fallen.",
                raiseOffered ? "Someone has offered you a raise." : "Wait here for a raise, or return",
                raiseOffered ? "" : "to your home point.",
            };

            constexpr float kTitleScale = 0.55f;
            constexpr float kBodyScale = 0.40f;
            constexpr float kLabelScale = 0.44f;

            int rows = 0;
            float widest = 0.0f;   // the widest row, in NDC x

            for (int i = 0; i < 3; ++i)
            {
                if (said[i][0] == '\0')
                {
                    continue;
                }

                const float scale = i == 0 ? kTitleScale : kBodyScale;
                const float cells = layOutRow(box, rows, said[i]);
                const float* tint = i == 0 ? kDialogTitle : kDialogText;
                box.colours[rows][0] = tint[0];
                box.colours[rows][1] = tint[1];
                box.colours[rows][2] = tint[2];
                box.colours[rows][3] = scale;
                widest = std::max(widest, cells * line * scale / windowAspect);
                ++rows;
            }

            // The two answers. "Accept Raise" is drawn whether or not one has
            // been offered: a button that appears out of nowhere is a button
            // nobody was watching for, and a dead player wants to know that
            // waiting is a thing that can end.
            const int firstButton = rows;
            const char* labels[mh::kDialogButtons] = {"Return to Home Point", "Accept Raise"};
            const mh::DeathChoice choices[mh::kDialogButtons] = {mh::DeathChoice::HomePoint,
                                                                 mh::DeathChoice::AcceptRaise};
            const bool live[mh::kDialogButtons] = {true, raiseOffered};

            float labelWidest = 0.0f;
            for (int i = 0; i < mh::kDialogButtons && rows < mh::kDialogRows; ++i)
            {
                const float cells = layOutRow(box, rows, labels[i]);
                const float* tint = live[i] ? kDialogLabel : kDialogLabelOff;
                box.colours[rows][0] = tint[0];
                box.colours[rows][1] = tint[1];
                box.colours[rows][2] = tint[2];
                box.colours[rows][3] = kLabelScale;
                labelWidest = std::max(labelWidest, cells * line * kLabelScale / windowAspect);
                ++rows;
            }

            // Sized to what it has to say rather than to a number picked once.
            // The lines change with the state and the font is proportional, so
            // a fixed box would either clip a sentence or stand half empty.
            const float padX = line * 0.85f / windowAspect;
            const float padY = line * 0.45f;
            const float titleGap = line * 0.42f;
            const float bodyGap = line * 0.16f;
            const float buttonsGap = line * 0.60f;

            const float buttonHigh = line * kLabelScale * 2.0f;
            const float buttonWide = labelWidest + padX * 1.3f;
            const float buttonGap = line * 0.45f / windowAspect;
            const float buttonsWide =
                buttonWide * mh::kDialogButtons + buttonGap * (mh::kDialogButtons - 1);

            // How much room to leave under each text row. The last one before
            // the buttons gets the wider gap, which is what separates what the
            // box says from what it is asking.
            const auto gapAfter = [&](int row)
            { return row == firstButton - 1 ? buttonsGap : (row == 0 ? titleGap : bodyGap); };

            float contentHigh = buttonHigh;
            for (int row = 0; row < firstButton; ++row)
            {
                contentHigh += line * box.colours[row][3] + gapAfter(row);
            }

            const float panelWide = std::max(widest, buttonsWide) + padX * 2.0f;
            const float panelHigh = contentHigh + padY * 2.0f;
            const float panelLeft = -panelWide * 0.5f;
            const float panelBottom = -panelHigh * 0.5f;

            box.panel[0] = panelLeft;
            box.panel[1] = panelBottom;
            box.panel[2] = panelWide;
            box.panel[3] = panelHigh;

            // Where the pointer is, so whatever it is over can light up. Read
            // here rather than tracked through motion events: it is one call,
            // and it cannot fall out of step with the box it is tested against.
            float pointerX = -2.0f;
            float pointerY = -2.0f;
            {
                float mouseX = 0.0f;
                float mouseY = 0.0f;
                SDL_GetMouseState(&mouseX, &mouseY);
                if (!pointerNdc(mouseX, mouseY, pointerX, pointerY))
                {
                    pointerX = -2.0f;   // off the window, so nothing is under it
                    pointerY = -2.0f;
                }
            }

            float cursorY = panelBottom + panelHigh - padY;
            for (int row = 0; row < firstButton; ++row)
            {
                const float high = line * box.colours[row][3];
                cursorY -= high;
                const float wide = box.boxes[row][2] * high / windowAspect;
                box.boxes[row][0] = -wide * 0.5f;   // centred, as the panel is
                box.boxes[row][1] = cursorY;
                cursorY -= gapAfter(row);
            }

            cursorY -= buttonHigh;
            for (int i = 0; i < mh::kDialogButtons; ++i)
            {
                const int row = firstButton + i;
                if (row >= mh::kDialogRows)
                {
                    break;
                }

                const float left = -buttonsWide * 0.5f + (buttonWide + buttonGap) * static_cast<float>(i);
                const bool under = live[i] && pointerX >= left && pointerX < left + buttonWide &&
                                   pointerY >= cursorY && pointerY < cursorY + buttonHigh;
                const float* fill = !live[i]                       ? kDialogButtonOff
                                    : (under || deathPressed == i) ? kDialogButtonHot
                                                                   : kDialogButton;

                box.rects[row][0] = left;
                box.rects[row][1] = cursorY;
                box.rects[row][2] = buttonWide;
                box.rects[row][3] = buttonHigh;
                box.fills[row][0] = fill[0];
                box.fills[row][1] = fill[1];
                box.fills[row][2] = fill[2];
                box.fills[row][3] = 1.0f;

                // The label, centred in its button both ways.
                const float high = line * box.colours[row][3];
                const float wide = box.boxes[row][2] * high / windowAspect;
                box.boxes[row][0] = left + (buttonWide - wide) * 0.5f;
                box.boxes[row][1] = cursorY + (buttonHigh - high) * 0.5f;

                deathButtons[i] = DialogButton{left, cursorY, buttonWide, buttonHigh, live[i], choices[i]};
            }

            deathPanel[0] = panelLeft;
            deathPanel[1] = panelBottom;
            deathPanel[2] = panelWide;
            deathPanel[3] = panelHigh;
            deathBoxShown = true;

            box.counts[0] = static_cast<float>(rows);
            queue.WriteBuffer(dialogUniformBuffer, 0, &box, sizeof(box));
            pass.SetPipeline(dialogPipeline);
            pass.SetBindGroup(0, dialogBindGroup);
            pass.Draw(3);
        }

        pass.End();

        // A texture copy has to start on a 256-byte row, so the readback is
        // padded and the padding is skipped when the rows are written out.
        const uint32_t bytesPerRow = (width * 4 + 255) / 256 * 256;
        const bool takingShot = screenshotPath && ++shotIndex >= 0 && (sequenceCount == 0 || shotIndex < sequenceCount);
        char shotPath[1024] = {};
        if (takingShot)
        {
            if (sequenceCount > 0)
            {
                std::snprintf(shotPath, sizeof(shotPath), screenshotPath, shotIndex);
            }
            else
            {
                std::snprintf(shotPath, sizeof(shotPath), "%s", screenshotPath);
            }
        }
        if (takingShot)
        {
            wgpu::BufferDescriptor readbackDescriptor{.usage = wgpu::BufferUsage::CopyDst |
                                                               wgpu::BufferUsage::MapRead,
                                                      .size = static_cast<uint64_t>(bytesPerRow) * height};
            readbackBuffer = device.CreateBuffer(&readbackDescriptor);

            wgpu::TexelCopyTextureInfo source{.texture = surfaceTexture.texture};
            wgpu::TexelCopyBufferInfo destination{
                .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = height}, .buffer = readbackBuffer};
            const wgpu::Extent3D extent{width, height, 1};
            encoder.CopyTextureToBuffer(&source, &destination, &extent);
        }

        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        surface.Present();
        instance.ProcessEvents();

        if (takingShot)
        {
            bool mapped = false;
            wgpu::Future future = readbackBuffer.MapAsync(
                wgpu::MapMode::Read, 0, wgpu::kWholeMapSize, wgpu::CallbackMode::AllowProcessEvents,
                [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                { mapped = status == wgpu::MapAsyncStatus::Success; });
            instance.WaitAny(future, UINT64_MAX);

            if (mapped)
            {
                const auto* pixels = static_cast<const uint8_t*>(readbackBuffer.GetConstMappedRange());
                if (pixels && writeBmp(shotPath, pixels, width, height, bytesPerRow))
                {
                    // The character height goes with it: a fall is far easier to
                    // read as a column of numbers than as a strip of pictures.
                    std::printf("wrote %s (%ux%u) characterY=%.2f\n", shotPath, width, height,
                                characterAt.y);
                }
                else
                {
                    std::printf("could not write %s\n", shotPath);
                }
                readbackBuffer.Unmap();
            }
            else
            {
                std::printf("could not read the frame back\n");
            }

            if (sequenceCount == 0 || shotIndex + 1 >= sequenceCount)
            {
                break;
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
