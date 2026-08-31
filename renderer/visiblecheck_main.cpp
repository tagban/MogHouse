// Asks whether the world you see matches the world you walk on.
//
// The collision geometry is trustworthy - it agrees with the server's own NPC
// placements, zone by zone (tools/npcground.py). The drawn geometry is a
// different table read by different code, and the two had drifted apart
// without any check noticing, because every check compared them from directly
// above. A top-down comparison cannot see a floor drawn at the wrong height,
// which is exactly what leaves a character hovering.
//
// So this samples the zone on a grid and, wherever collision says there is a
// floor, looks for a drawn surface near it. No GPU, no window: it builds the
// same Scene the renderer would and intersects it on the CPU.
//
//     ffxi-visiblecheck <zone.DAT> [spacing]

#include "collision.h"
#include "ffxi/dat.h"
#include "ffxi/mmb.h"
#include "ffxi/mzb.h"
#include "linalg.h"
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
/// A drawn triangle, already placed in the world.
struct Tri
{
    mh::Vec3 a, b, c;
};

/// Where the plane of a triangle sits over (x, z), or nothing if the point is
/// outside its footprint. The same barycentric test collision.cpp uses.
std::optional<float> heightAt(const mh::Vec3& a, const mh::Vec3& b, const mh::Vec3& c, float x, float z)
{
    const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::fabs(d) < 1e-9f)
    {
        return std::nullopt;
    }
    const float w0 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
    const float w1 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
    const float w2 = 1.0f - w0 - w1;
    const float slack = -0.0005f;
    if (w0 < slack || w1 < slack || w2 < slack)
    {
        return std::nullopt;
    }
    return w0 * a.y + w1 * b.y + w2 * c.y;
}

/// Drawn triangles bucketed by their x/z footprint, so a query looks at a
/// handful rather than at every triangle in the zone.
class Index
{
public:
    Index(const std::vector<Tri>& tris, mh::Vec3 lo, mh::Vec3 hi) : tris_(tris), lo_(lo)
    {
        cols_ = std::max(1, static_cast<int>((hi.x - lo.x) / kCell) + 1);
        rows_ = std::max(1, static_cast<int>((hi.z - lo.z) / kCell) + 1);
        cells_.resize(static_cast<size_t>(cols_) * rows_);

        for (uint32_t i = 0; i < tris.size(); ++i)
        {
            const Tri& t = tris[i];
            const int x0 = col(std::min({t.a.x, t.b.x, t.c.x}));
            const int x1 = col(std::max({t.a.x, t.b.x, t.c.x}));
            const int z0 = row(std::min({t.a.z, t.b.z, t.c.z}));
            const int z1 = row(std::max({t.a.z, t.b.z, t.c.z}));
            // A triangle spanning a huge span is usually a skybox or a ground
            // plate; letting it into every cell it touches costs more than it
            // is worth, but dropping it would hide real geometry, so keep it.
            for (int z = z0; z <= z1; ++z)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    cells_[static_cast<size_t>(z) * cols_ + x].push_back(i);
                }
            }
        }
    }

    /// The drawn surface whose height at (x, z) is closest to `want`.
    std::optional<float> nearest(float x, float z, float want) const
    {
        const int cx = col(x);
        const int cz = row(z);
        if (cx < 0 || cz < 0 || cx >= cols_ || cz >= rows_)
        {
            return std::nullopt;
        }
        std::optional<float> best;
        for (uint32_t i : cells_[static_cast<size_t>(cz) * cols_ + cx])
        {
            const Tri& t = tris_[i];
            const std::optional<float> y = heightAt(t.a, t.b, t.c, x, z);
            if (y && (!best || std::fabs(*y - want) < std::fabs(*best - want)))
            {
                best = y;
            }
        }
        return best;
    }

private:
    static constexpr float kCell = 8.0f;
    int col(float x) const { return std::clamp(static_cast<int>((x - lo_.x) / kCell), 0, cols_ - 1); }
    int row(float z) const { return std::clamp(static_cast<int>((z - lo_.z) / kCell), 0, rows_ - 1); }

    const std::vector<Tri>& tris_;
    mh::Vec3 lo_;
    int cols_{};
    int rows_{};
    std::vector<std::vector<uint32_t>> cells_;
};

mh::Vec3 apply(const float* m, const mh::Vec3& p)
{
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-visiblecheck <zone.DAT> [spacing]\n");
        return 2;
    }
    const float spacing = argc >= 3 ? static_cast<float>(std::atof(argv[2])) : 2.0f;

    const char* keyPath = std::getenv("MOGHOUSE_FFXI_KEYTABLE");
    const char* key2Path = std::getenv("MOGHOUSE_FFXI_KEYTABLE2");
    auto keys = keyPath ? ffxi::KeyTable::load(keyPath) : std::nullopt;
    auto keys2 = key2Path ? ffxi::KeyTable::load(key2Path) : std::nullopt;
    if (!keys || !keys2)
    {
        std::printf("set MOGHOUSE_FFXI_KEYTABLE and MOGHOUSE_FFXI_KEYTABLE2\n");
        return 2;
    }

    ffxi::DatFile dat{std::filesystem::path{argv[1]}};

    std::unordered_map<std::string, ffxi::Model> models;
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
        }
    }
    std::printf("%zu models\n", models.size());

    const std::unordered_map<std::string, ffxi::Texture> noTextures;

    // --points reads "x y z name" a line on stdin, in FFXI's own coordinates,
    // and reports how far the nearest drawn triangle is from each. The server's
    // door and NPC placements are the outside reference this needs: they say
    // where a thing belongs, authored against the real geometry, by someone
    // other than us.
    const bool pointMode = argc >= 3 && std::string{argv[2]} == "--points";
    std::vector<std::pair<mh::Vec3, std::string>> wanted;
    if (pointMode)
    {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        char name[128];
        while (std::scanf("%f %f %f %127[^\n]", &px, &py, &pz, name) == 4)
        {
            // The same half turn about X the world is built with.
            wanted.push_back({mh::Vec3{px, -py, -pz}, std::string{name}});
        }
    }

    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);
        mh::Collision collision{zone};
        if (collision.empty())
        {
            continue;
        }

        size_t resolved = 0;
        size_t missing = 0;
        const mh::Scene scene = mh::buildScene(zone, models, noTextures, resolved, missing);

        // Every drawn triangle, placed. This is the geometry the GPU would
        // receive, expanded here so it can be queried.
        std::vector<Tri> tris;
        tris.reserve(scene.drawnTriangles());
        for (const mh::InstancedDraw& draw : scene.draws)
        {
            for (uint32_t n = 0; n < draw.instanceCount; ++n)
            {
                const float* m = &scene.instances[(static_cast<size_t>(draw.instanceOffset) + n) * 16];
                for (uint32_t i = 0; i + 2 < draw.indexCount; i += 3)
                {
                    const mh::Vertex& va = scene.vertices[scene.indices[draw.indexOffset + i]];
                    const mh::Vertex& vb = scene.vertices[scene.indices[draw.indexOffset + i + 1]];
                    const mh::Vertex& vc = scene.vertices[scene.indices[draw.indexOffset + i + 2]];
                    tris.push_back({apply(m, {va.position[0], va.position[1], va.position[2]}),
                                    apply(m, {vb.position[0], vb.position[1], vb.position[2]}),
                                    apply(m, {vc.position[0], vc.position[1], vc.position[2]})});
                }
            }
        }

        const mh::Vec3 lo = collision.boundsMin();
        const mh::Vec3 hi = collision.boundsMax();

        if (pointMode)
        {
            std::printf("%zu points against %zu drawn triangles\n", wanted.size(), tris.size());
            for (const auto& [at, name] : wanted)
            {
                float best = std::numeric_limits<float>::max();
                for (const Tri& t2 : tris)
                {
                    // Nearest vertex is close enough to say whether anything is
                    // there at all, and far cheaper than a true point-triangle
                    // distance over a hundred thousand of them.
                    for (const mh::Vec3& v : {t2.a, t2.b, t2.c})
                    {
                        const float dx = v.x - at.x;
                        const float dy = v.y - at.y;
                        const float dz = v.z - at.z;
                        best = std::min(best, dx * dx + dy * dy + dz * dz);
                    }
                }
                std::printf("  %7.2f  %8.1f %7.1f %8.1f  %s\n", std::sqrt(best), at.x, at.y, at.z, name.c_str());
            }
            continue;
        }
        std::printf("zone %s: %zu placements, %zu drawn triangles\n", zone.id.c_str(), resolved, tris.size());
        std::printf("  collision bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", lo.x, hi.x, lo.y, hi.y, lo.z,
                    hi.z);

        const Index index{tris, lo, hi};

        // Everything is measured against the floor collision reports, because
        // that is the side the server agrees with.
        constexpr float kNear = 0.6f;   // drawn close enough to be that floor
        constexpr float kBadly = 3.0f;  // drawn, but nowhere near

        size_t floors = 0;
        size_t matched = 0;
        size_t offset = 0;
        size_t absent = 0;

        // Worst offenders, gathered into blocks so the report names places
        // rather than thousands of points.
        std::map<std::pair<int, int>, std::pair<size_t, size_t>> blocks;
        constexpr float kBlock = 32.0f;

        for (float z = lo.z; z <= hi.z; z += spacing)
        {
            for (float x = lo.x; x <= hi.x; x += spacing)
            {
                const std::optional<float> floor = collision.groundAt(x, z, hi.y + 10.0f);
                if (!floor)
                {
                    continue;
                }

                // Skip the plate along the bottom of the world. A zone has a
                // floor under everything - beneath cliffs, behind walls, under
                // the buildings themselves - and nothing is drawn down there
                // because nothing should be. Counting it buried the real
                // defects under whole districts of false positives that render
                // perfectly well when you go and look at them.
                if (*floor <= lo.y + 0.5f)
                {
                    continue;
                }
                ++floors;

                const std::optional<float> drawn = index.nearest(x, z, *floor);
                const bool bad = !drawn || std::fabs(*drawn - *floor) > kBadly;
                if (!drawn)
                {
                    ++absent;
                }
                else if (std::fabs(*drawn - *floor) <= kNear)
                {
                    ++matched;
                }
                else
                {
                    ++offset;
                }

                auto& block = blocks[{static_cast<int>(std::floor(x / kBlock)),
                                      static_cast<int>(std::floor(z / kBlock))}];
                ++block.first;
                if (bad)
                {
                    ++block.second;
                }
            }
        }

        if (floors == 0)
        {
            continue;
        }
        std::printf("  %zu sampled points with a collision floor\n", floors);
        std::printf("    %6zu (%5.1f%%) have a drawn surface within %.1f of it\n", matched,
                    100.0 * matched / floors, kNear);
        std::printf("    %6zu (%5.1f%%) have one, but further away\n", offset, 100.0 * offset / floors);
        std::printf("    %6zu (%5.1f%%) have nothing drawn at all\n", absent, 100.0 * absent / floors);

        std::vector<std::tuple<double, size_t, int, int>> worst;
        for (const auto& [cell, counts] : blocks)
        {
            if (counts.first >= 40)
            {
                worst.emplace_back(static_cast<double>(counts.second) / counts.first, counts.first, cell.first,
                                   cell.second);
            }
        }
        std::sort(worst.rbegin(), worst.rend());

        std::printf("\n  worst 32-unit blocks - collision floor with nothing drawn near it\n");
        std::printf("     bad   points    world x, z\n");
        for (size_t i = 0; i < worst.size() && i < 14; ++i)
        {
            const auto& [share, points, cx, cz] = worst[i];
            if (share <= 0.0)
            {
                break;
            }
            std::printf("   %5.1f%%  %7zu   %8.1f %8.1f\n", share * 100.0, points, (cx + 0.5f) * kBlock,
                        (cz + 0.5f) * kBlock);
        }
    }

    return 0;
}
