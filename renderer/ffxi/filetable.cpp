#include "filetable.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ffxi
{
namespace
{
std::vector<uint8_t> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file)
    {
        throw std::runtime_error("could not open " + path.string());
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    return buffer;
}
} // namespace

FileTable::FileTable(std::filesystem::path installRoot) : root_(std::move(installRoot))
{
    vtable_ = readWholeFile(root_ / "VTABLE.DAT");
    ftable_ = readWholeFile(root_ / "FTABLE.DAT");
    if (ftable_.size() != vtable_.size() * 2)
    {
        throw std::runtime_error("VTABLE and FTABLE disagree on how many ids there are");
    }
}

std::optional<std::filesystem::path> FileTable::path(size_t fileId) const
{
    if (fileId >= vtable_.size())
    {
        return std::nullopt;
    }
    const uint8_t rom = vtable_[fileId];
    if (rom == 0)
    {
        return std::nullopt;
    }

    uint16_t packed = 0;
    std::memcpy(&packed, ftable_.data() + fileId * 2, sizeof(packed));

    // ROM 1 lives in a folder called plain "ROM"; the rest carry their number.
    const std::string folder = rom == 1 ? "ROM" : "ROM" + std::to_string(rom);
    return root_ / folder / std::to_string(packed >> 7) / (std::to_string(packed & 0x7F) + ".DAT");
}

std::filesystem::path defaultInstallRoot()
{
    if (const char* fromEnv = std::getenv("MOGHOUSE_FFXI_INSTALL"))
    {
        return std::filesystem::path{fromEnv};
    }
    return std::filesystem::path{"C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"};
}
} // namespace ffxi
