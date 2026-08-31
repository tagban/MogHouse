#pragma once

// Names for the things in a zone, which the server does not send.
//
// An NPC arrives from the server with a position, a look and an empty name -
// the name lives in the client's own files, one table per zone, which is why a
// server database can have no row for a zone the client still populates.
//
// The table is file id 6720 + zone, a flat run of 32-byte records: 28 bytes of
// NUL-padded name, then the entity id the server uses. That id is
// 0x1000000 | zone << 12 | targid, so a record identifies itself and the
// mapping was checked by reading four zones back and confirming every id in
// each names its own zone.

#include "filetable.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ffxi
{
/// The file id holding a zone's entity names.
inline constexpr size_t kEntityNameBase = 6720;

/// Entity id to name, for one zone.
class EntityNames
{
public:
    EntityNames() = default;

    /// Reads the table for one zone. An absent or unreadable file leaves this
    /// empty, which costs nameplates rather than the zone.
    static EntityNames load(const FileTable& table, uint16_t zoneId);

    /// The name for an entity id, or an empty string. Ids the table does not
    /// carry are normal: players are not in it.
    const std::string& lookup(uint32_t entityId) const;

    size_t size() const { return names_.size(); }
    bool empty() const { return names_.empty(); }

private:
    std::unordered_map<uint32_t, std::string> names_;
};
} // namespace ffxi
