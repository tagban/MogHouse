#include "filetable.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
// For the registry, which is where PlayOnline records the install path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

#ifdef _WIN32
namespace
{
/// The install path PlayOnline recorded, or empty.
///
/// Under InstallFolder the values are numbered rather than named: 0001 is Final
/// Fantasy XI and 1000 is the PlayOnline viewer. Both the US and the JP/EU keys
/// are worth asking, and a 32-bit installer on a 64-bit machine lands under
/// WOW6432Node - which is where it actually is on a normal install.
std::filesystem::path installFromRegistry()
{
    static const wchar_t* kKeys[] = {
        L"SOFTWARE\\WOW6432Node\\PlayOnlineUS\\InstallFolder",
        L"SOFTWARE\\WOW6432Node\\PlayOnline\\InstallFolder",
        L"SOFTWARE\\PlayOnlineUS\\InstallFolder",
        L"SOFTWARE\\PlayOnline\\InstallFolder",
    };

    for (const wchar_t* key : kKeys)
    {
        HKEY handle = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle) != ERROR_SUCCESS)
        {
            continue;
        }

        wchar_t value[MAX_PATH] = {};
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LSTATUS read = RegQueryValueExW(handle, L"0001", nullptr, &type,
                                              reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(handle);

        if (read == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0')
        {
            std::filesystem::path found{value};
            if (std::filesystem::exists(found))
            {
                return found;
            }
        }
    }

    return {};
}
} // namespace
#endif

std::filesystem::path defaultInstallRoot()
{
    if (const char* fromEnv = std::getenv("MOGHOUSE_FFXI_INSTALL"))
    {
        return std::filesystem::path{fromEnv};
    }

#ifdef _WIN32
    // Ask PlayOnline where it put the game rather than guessing. The guess
    // below is only right for a default install on the C drive, which is no
    // use to anyone running this on a machine that is not the one it was
    // written on.
    if (std::filesystem::path recorded = installFromRegistry(); !recorded.empty())
    {
        return recorded;
    }
#endif

    return std::filesystem::path{"C:/Program Files (x86)/PlayOnline/SquareEnix/FINAL FANTASY XI"};
}
} // namespace ffxi
