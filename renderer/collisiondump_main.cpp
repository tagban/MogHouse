// Probes a zone's collision geometry, without a GPU.
//
// The ground query either answers or it does not, and finding out which from
// inside a render loop means a two-minute rebuild per guess.

#include "collision.h"
#include "ffxi/dat.h"
#include "ffxi/mzb.h"

#include <cmath>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-collisiondump <zone.DAT> [x z]\n");
        return 2;
    }

    const char* keyPath = std::getenv("MOGHOUSE_FFXI_KEYTABLE");
    auto keys = keyPath ? ffxi::KeyTable::load(keyPath) : std::nullopt;
    if (!keys)
    {
        std::printf("set MOGHOUSE_FFXI_KEYTABLE to the 256-byte MZB key table\n");
        return 2;
    }

    ffxi::DatFile dat{std::filesystem::path{argv[1]}};
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);
        mh::Collision collision{zone};

        std::printf("zone %s: %zu collision meshes, %zu instances -> %zu triangles\n", zone.id.c_str(),
                    zone.collision.size(), zone.instances.size(), collision.triangleCount());
        std::printf("  %zu walls, %zu walkable\n", collision.wallCount(),
                    collision.triangleCount() - collision.wallCount());
        if (collision.empty())
        {
            continue;
        }
        const mh::Vec3 lo = collision.boundsMin();
        const mh::Vec3 hi = collision.boundsMax();
        std::printf("  bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);

        // Walk from a point in a straight line, the way the renderer does, and
        // report where the ground goes and where a wall stops it.
        if (argc >= 6)
        {
            const float startX = static_cast<float>(std::atof(argv[2]));
            const float startZ = static_cast<float>(std::atof(argv[3]));
            const float dirX = static_cast<float>(std::atof(argv[4]));
            const float dirZ = static_cast<float>(std::atof(argv[5]));

            std::optional<mh::Vec3> at = collision.nearestGround(startX, startZ, hi.y + 10.0f, 60.0f);
            if (!at)
            {
                std::printf("  no ground within 60 units of %.1f %.1f\n", startX, startZ);
                continue;
            }
            std::printf("  start %.1f %.1f %.1f\n", at->x, at->y, at->z);

            size_t blocked = 0;
            for (int step = 0; step < 40; ++step)
            {
                const mh::Vec3 wanted{at->x + dirX, at->y, at->z + dirZ};
                const mh::Vec3 stepped = collision.move(*at, wanted, 0.5f);
                const float dx = stepped.x - at->x;
                const float dz = stepped.z - at->z;
                const bool stopped = std::sqrt(dx * dx + dz * dz) < 1e-4f;

                std::optional<mh::Vec3> next =
                    collision.nearestGround(stepped.x, stepped.z, at->y + 1.0f, 4.0f);
                if (!next)
                {
                    std::printf("  step %2d: walked off the edge at %.1f %.1f\n", step, stepped.x, stepped.z);
                    break;
                }
                if (stopped)
                {
                    ++blocked;
                }
                if (stopped)
                {
                    const std::vector<mh::Collision::Blocker> blockers = collision.blockersNear(*at, 2.0f);
                    std::printf("  %zu wall triangles straddle the character here\n", blockers.size());
                    for (size_t i = 0; i < blockers.size() && i < 4; ++i)
                    {
                        std::printf("    normal %.2f %.2f %.2f   spans y %.1f..%.1f\n", blockers[i].normal.x,
                                    blockers[i].normal.y, blockers[i].normal.z, blockers[i].lowest,
                                    blockers[i].highest);
                    }
                }
                if (step % 5 == 0 || stopped)
                {
                    std::printf("  step %2d: %.1f %.1f %.1f%s\n", step, next->x, next->y, next->z,
                                stopped ? "   BLOCKED" : "");
                }
                at = next;
                if (blocked > 2)
                {
                    std::printf("  stopped by a wall\n");
                    break;
                }
            }
            continue;
        }

        if (argc >= 4)
        {
            const float x = static_cast<float>(std::atof(argv[2]));
            const float z = static_cast<float>(std::atof(argv[3]));
            for (float from : {1000.0f, 200.0f, 120.0f, 80.0f, 0.0f})
            {
                const std::optional<float> ground = collision.groundAt(x, z, from);
                std::printf("  groundAt(%.1f, %.1f) looking down from %.0f: %s\n", x, z, from,
                            ground ? std::to_string(*ground).c_str() : "nothing");
            }
            continue;
        }

        // How much of the zone answers at all. A query that works should hit
        // on most of the interior and miss outside the playable area.
        size_t probes = 0;
        size_t hits = 0;
        for (int ix = 0; ix < 60; ++ix)
        {
            for (int iz = 0; iz < 60; ++iz)
            {
                const float x = lo.x + (hi.x - lo.x) * (ix + 0.5f) / 60.0f;
                const float z = lo.z + (hi.z - lo.z) * (iz + 0.5f) / 60.0f;
                ++probes;
                if (collision.groundAt(x, z, hi.y + 10.0f))
                {
                    ++hits;
                }
            }
        }
        std::printf("  %zu of %zu probes found ground (%.0f%%)\n", hits, probes,
                    100.0 * static_cast<double>(hits) / static_cast<double>(probes));

        // The walkability mask, as a plain grey PGM - no encoder needed and
        // anything can open it.
        {
            const float half = std::max(hi.x - lo.x, hi.z - lo.z) * 0.5f;
            const mh::Vec3 middle{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
            constexpr uint32_t kSize = 1024;
            const std::vector<uint8_t> mask = collision.rasteriseWalkable(kSize, middle, half);
            size_t set = 0;
            for (uint8_t value : mask)
            {
                set += value ? 1 : 0;
            }
            std::printf("  walkable mask: %zu of %zu texels (%.0f%%)\n", set, mask.size(),
                        100.0 * static_cast<double>(set) / static_cast<double>(mask.size()));
            if (const char* out = std::getenv("MOGHOUSE_MASK"))
            {
                if (std::FILE* file = std::fopen(out, "wb"))
                {
                    std::fprintf(file, "P5\n%u %u\n255\n", kSize, kSize);
                    std::fwrite(mask.data(), 1, mask.size(), file);
                    std::fclose(file);
                    std::printf("  wrote %s\n", out);
                }
            }
        }

        // Standing somewhere is not the same as being able to go anywhere. From
        // each spot with ground, try to walk ten units in four directions and
        // see how many get there. A zone where most ground is walkable in most
        // directions is working; one where it is not means the wall test is too
        // eager, which a hit rate alone will not show.
        size_t open = 0;
        size_t tested = 0;
        mh::Vec3 bestSpot{};
        int bestScore = -1;
        for (int ix = 0; ix < 40; ++ix)
        {
            for (int iz = 0; iz < 40; ++iz)
            {
                const float x = lo.x + (hi.x - lo.x) * (ix + 0.5f) / 40.0f;
                const float z = lo.z + (hi.z - lo.z) * (iz + 0.5f) / 40.0f;
                const std::optional<float> y = collision.groundAt(x, z, hi.y + 10.0f);
                if (!y)
                {
                    continue;
                }
                ++tested;

                int reached = 0;
                const float dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& dir : dirs)
                {
                    mh::Vec3 at{x, *y, z};
                    for (int step = 0; step < 10; ++step)
                    {
                        const mh::Vec3 stepped = collision.move(at, {at.x + dir[0], at.y, at.z + dir[1]}, 0.5f);
                        const std::optional<mh::Vec3> next =
                            collision.nearestGround(stepped.x, stepped.z, at.y + 1.0f, 2.0f);
                        if (!next)
                        {
                            break;
                        }
                        at = *next;
                    }
                    const float dx = at.x - x;
                    const float dz = at.z - z;
                    if (std::sqrt(dx * dx + dz * dz) > 7.0f)
                    {
                        ++reached;
                    }
                }
                if (reached >= 3)
                {
                    ++open;
                }
                if (reached > bestScore)
                {
                    bestScore = reached;
                    bestSpot = {x, *y, z};
                }
            }
        }
        std::printf("  %zu of %zu standable spots walk 10 units in 3+ directions (%.0f%%)\n", open, tested,
                    tested ? 100.0 * static_cast<double>(open) / static_cast<double>(tested) : 0.0);
        std::printf("  most open spot: %.1f %.1f %.1f (%d of 4 directions)\n", bestSpot.x, bestSpot.y, bestSpot.z,
                    bestScore);
    }
    return 0;
}
