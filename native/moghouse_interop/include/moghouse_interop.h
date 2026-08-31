// The C ABI between the MogHouse renderer and the C# client.
//
// MogHouse ships as one application. The renderer is C++ because the asset
// pipeline is; the game logic is C# because the protocol work is. This is the
// seam, and it is plain C so neither side needs the other's toolchain.
//
// Deliberately narrow. Everything the client has to say to the renderer today
// is "open this zone", "here is what is nearby" and "please close". Anything
// wider would be guessing at what the two halves will want of each other.
#ifndef MOGHOUSE_INTEROP_H
#define MOGHOUSE_INTEROP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MH_API __declspec(dllexport)
#else
#define MH_API __attribute__((visibility("default")))
#endif

typedef struct MhViewer* MhViewerHandle;

/// Kinds match MogHouse.Core's FfxiEntityKind, so the C# side can cast.
enum
{
    MH_ENTITY_PLAYER = 0,
    MH_ENTITY_NPC = 1,
    MH_ENTITY_ENEMY = 2
};

/// One tracked thing, in world coordinates. The radar uses x and z; the rest
/// is what it takes to stand a body there and point it somewhere.
///
/// Y is up, as everywhere past the DAT readers, and heading is radians with 0
/// along +z. The client converts out of FFXI's own frame on the way in.
typedef struct MhRadarEntity
{
    float x;
    float z;
    float y;
    float heading;
    int32_t kind;

    /// Shown over the body, NUL terminated, ASCII. Fixed width so the whole
    /// array stays blittable and crosses as a pointer - a char* per entity
    /// would mean owning lifetimes across a thread boundary for no gain.
    char name[20];

    /// The server's id, 0x1000000 | zone << 12 | targid. Zero means unknown.
    /// Used to find a name in the zone's own name table when the server sends
    /// none, which for NPCs it always does.
    uint32_t id;

    /// Non-zero when the server wants the name kept off screen until this is
    /// targeted. Doors and zone lines are named but not labelled.
    int32_t nameHidden;

    /// What the entity looks like, when the server describes it the way it
    /// describes a player: race, face, then the head, body, hands, legs and
    /// feet model ids with their slot tags already stripped. All zero means
    /// the server did not describe it this way - a fixed model, a door - and
    /// the shared body stands in.
    uint16_t look[7];

    /// GM level, 0 for an ordinary player. Only the name's colour uses it.
    int32_t gmLevel;
} MhRadarEntity;

/// What to open. Every string is borrowed for the duration of the create call
/// and copied, so the caller may free them straight afterwards. A null string
/// means "not set", which for several of these is different from empty.
typedef struct MhViewerOptions
{
    const char* zone_path;
    const char* key_table_path;
    const char* key_table2_path;

    /// "race,face,head,body,hands,legs,feet", or null for no character.
    const char* look;

    /// The server's Vana'diel clock in seconds, or 0 to run the renderer's own.
    uint32_t server_clock;

    /// "x,y,z" to stand the character at, or null to pick somewhere.
    const char* character_at;

    /// Compass degrees, or null.
    const char* character_facing;

    /// Shown along the bottom of the radar. The renderer has no zone-name
    /// table; the caller already knows what it asked for.
    const char* zone_name;

    /// The name over our own character's head. The game shows everyone their
    /// own nameplate, and the renderer has no idea who it is playing.
    const char* player_name;

    /// Vana'diel clock as hhmm, or -1 to let the day run.
    int32_t time_of_day;

    /// Writes one frame to this path and closes, or null to stay open. Meant
    /// for checking what the viewer produced without someone watching it.
    const char* screenshot_path;

    /// Frames to wait first. A shot taken before the caller has posted
    /// anything shows an empty radar and proves nothing.
    int32_t screenshot_after_frames;
} MhViewerOptions;

/// Creates a viewer. Does not open a window - see mh_viewer_run.
MH_API MhViewerHandle mh_viewer_create(const MhViewerOptions* options);

/// Opens the window and runs until it closes. Blocking, and it owns the event
/// loop while it runs, so callers give it a thread of its own. Returns a
/// process-style exit code: 0 for a clean close.
MH_API int32_t mh_viewer_run(MhViewerHandle viewer);

/// Replaces what the radar shows. Safe to call from another thread while
/// mh_viewer_run is going, which is the point of the whole file.
MH_API void mh_viewer_set_entities(MhViewerHandle viewer, const MhRadarEntity* entities, int32_t count);

/// One of this zone's exits, as somewhere to draw a marker.
///
/// Y is up, matching everything else that crosses this boundary, and the
/// radius is how close counts as touching the line.
typedef struct MhZoneLine
{
    float x;
    float y;
    float z;
    float radius;
} MhZoneLine;

/// Replaces the zone lines drawn in the world. Wholesale: a line belongs to
/// the zone it came from and means nothing on the other side.
MH_API void mh_viewer_set_zone_lines(MhViewerHandle viewer, const MhZoneLine* lines, int32_t count);

/// Adds one line to the chat panel. UTF-8 in, though the panel's font only
/// covers letters, digits and a little punctuation - anything else becomes a
/// space rather than a wrong glyph.
MH_API void mh_viewer_push_chat(MhViewerHandle viewer, const char* line);

/// Where the character has walked to. Returns 0 before the first frame.
///
/// The client owns the connection, so movement only reaches the server if it
/// asks for it. Y is up; FFXI's own vertical is the negation.
MH_API int32_t mh_viewer_get_character(MhViewerHandle viewer, float* x, float* y, float* z, float* heading);

/// Whether the player asked to jump since this was last called, clearing it.
///
/// The renderer animates the jump itself; this is how the client learns one
/// happened so it can tell the server, which is the only way anyone else sees
/// it.
MH_API int32_t mh_viewer_take_jump(MhViewerHandle viewer);

/// The next line the player typed, copied into `buffer`. Returns 0 when there
/// is nothing waiting, so a caller polls this the way it polls the jump.
MH_API int32_t mh_viewer_take_chat(MhViewerHandle viewer, char* buffer, int32_t capacity);

/// Puts the character somewhere, because the server said so. Y is up here, as
/// everywhere past the DAT readers; the caller converts.
MH_API void mh_viewer_place_character(MhViewerHandle viewer, float x, float y, float z, float heading);

/// Asks a running viewer to close. mh_viewer_run returns shortly after.
MH_API void mh_viewer_stop(MhViewerHandle viewer);

/// Frees the viewer. Stop it and let mh_viewer_run return first.
MH_API void mh_viewer_destroy(MhViewerHandle viewer);

#ifdef __cplusplus
}
#endif

#endif // MOGHOUSE_INTEROP_H
