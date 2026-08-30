// Reports what the C++ reader sees in a DAT, in the same shape as
// tools/mzbmesh.py, so the two can be compared. The python was checked against
// every zone in ROM/1; this exists so the C++ can be held to the same result.

#include "dat.h"
#include "mzb.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
                std::printf("zone %s  version=%#04x  placements=%zu  meshes=%zu\n", zone.id.c_str(), zone.version,
                            zone.placements.size(), zone.collision.size());
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
