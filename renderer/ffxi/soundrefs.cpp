#include "soundrefs.h"

#include "dat.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace ffxi
{
namespace
{
constexpr size_t kIdOffset = 8;   // past "SeSep  "

std::vector<SoundRef> collect(const DatFile& dat)
{
    std::vector<SoundRef> found;
    for (const Chunk& chunk : dat.chunks())
    {
        if (chunk.type != 0x3D || chunk.data.size() < kIdOffset + 4)
        {
            continue;
        }
        if (std::memcmp(chunk.data.data(), "SeSep", 5) != 0)
        {
            continue;
        }

        uint32_t id = 0;
        std::memcpy(&id, chunk.data.data() + kIdOffset, sizeof(id));

        std::string name(chunk.id, 4);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
        {
            name.pop_back();
        }

        found.push_back(SoundRef{std::move(name), id});
    }
    return found;
}
} // namespace

std::filesystem::path SoundRef::file() const
{
    char folder[16]{};
    char name[24]{};
    std::snprintf(folder, sizeof(folder), "se%03u", id / 1000);
    std::snprintf(name, sizeof(name), "se%06u.spw", id);
    return std::filesystem::path{"se"} / folder / name;
}

std::vector<SoundRef> soundReferences(const std::filesystem::path& datPath)
{
    try
    {
        return collect(DatFile{datPath});
    }
    catch (const std::exception&)
    {
        return {};
    }
}

bool isStandardCreatureSound(const std::string& name)
{
    // What every creature has. Counted across 614 models: anything outside
    // this is the interesting part, and is how the worm's two extras were
    // noticed.
    static const std::set<std::string> standard{
        "idl1", "idl2", "atk1", "atk2", "atk3", "atk4", "dam1", "dam2", "dam3", "dam4",
        "swy1", "swy2", "swy3", "ded1", "ded2", "ded3", "sdam", "skaz", "shit"};
    return standard.contains(name);
}
} // namespace ffxi
