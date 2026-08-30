#pragma once

// Resolves FFXI file ids to paths, the way the client does. See
// docs/file-index.md.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ffxi
{
/// The install's own index: a flat file id maps to a path through two tables
/// at the install root, one entry per id in each.
///
///     VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
///     FTABLE.DAT   u16  (directory << 7) | file
class FileTable
{
public:
    /// Throws std::runtime_error if the tables are missing or disagree.
    explicit FileTable(std::filesystem::path installRoot);

    size_t size() const { return vtable_.size(); }

    /// The path for a file id, or nothing if that id is not installed.
    std::optional<std::filesystem::path> path(size_t fileId) const;

private:
    std::filesystem::path root_;
    std::vector<uint8_t> vtable_;
    std::vector<uint8_t> ftable_;
};

/// Where the retail client is installed, from MOGHOUSE_FFXI_INSTALL, falling
/// back to the usual Windows location.
std::filesystem::path defaultInstallRoot();
} // namespace ffxi
