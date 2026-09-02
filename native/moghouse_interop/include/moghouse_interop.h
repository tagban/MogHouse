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

    /// A creature's model, when the server describes it as one fixed model
    /// rather than as a race wearing things: a rabbit, a crab, a goblin.
    /// Zero when it has none. See mh::creatureFileId.
    uint32_t modelId;

    /// Health, 0 to 100. A mob at zero is a corpse: still an entity, still
    /// named, and not something to attack. -1 when the server has not said.
    int32_t healthPercent;

    /// Non-zero when the server will accept a trigger on this entity, which is
    /// what makes it worth putting a cursor over.
    int32_t triggerable;
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

/// What a dead player pressed in the box the renderer draws them. Matches
/// mh::DeathChoice.
enum
{
    MH_DEATH_NONE = 0,
    MH_DEATH_HOME_POINT = 1,
    MH_DEATH_ACCEPT_RAISE = 2
};

/// Whether the character is down, and whether a raise has been offered.
///
/// Per session rather than per entity, which is why this is a call of its own
/// rather than another field on MhRadarEntity. The renderer cannot work either
/// out for itself: it knows where the body is and nothing about the state of
/// it. Hit points arrive in one packet and the raise offer in another, so both
/// are the caller's answer - and together they are the whole of what the box
/// draws itself from. It appears on the first and its second button lights on
/// the second.
MH_API void mh_viewer_set_death(MhViewerHandle viewer, int32_t dead, int32_t raise_offered);

/// The player's own HP, MP and TP, with the two percentages the server sends
/// beside them. Drawn as a panel in the world window - without it, being dead
/// is something you deduce from not being able to move.
MH_API void mh_viewer_set_vitals(MhViewerHandle viewer, uint32_t hp, uint32_t mp, uint32_t tp,
                                 uint8_t hp_percent, uint8_t mp_percent);

/// Which link the player clicked in the world window, taken once: 0 none,
/// 1 Discord, 2 the issue tracker. The renderer knows a corner was pressed and
/// nothing about browsers, so opening it is the caller's job.
MH_API int32_t mh_viewer_take_link(MhViewerHandle viewer);

/// The .bgw the zone wants playing, or null for silence. The server sends a
/// track number and the caller turns that into a path; the renderer owns the
/// audio device because that is where SDL already is.
MH_API void mh_viewer_set_music(MhViewerHandle viewer, const char* path);

/// Draws a different zone in the window already open, rather than closing it
/// and opening another. The position is where the character lands.
MH_API void mh_viewer_load_zone(MhViewerHandle viewer, const char* dat_path, const char* zone_name,
                                float x, float y, float z, float heading);

/// Whether a zone is being read right now. While it is, the position this
/// window reports still belongs to the zone being left, and sending that to the
/// server is how a zone change turns into a loop.
MH_API int32_t mh_viewer_is_loading(MhViewerHandle viewer);

/// Preferences, both ways. Set once when the world opens; read back after the
/// keys in the world window change them, so they can be written to disk.
/// mh_viewer_take_settings returns non-zero when something changed.
MH_API void mh_viewer_set_settings(MhViewerHandle viewer, float music_volume, int32_t radar_turns);
MH_API int32_t mh_viewer_take_settings(MhViewerHandle viewer, float* music_volume, int32_t* radar_turns);

/// What the player pressed there, as one of MH_DEATH_*, consumed by the read.
/// Both answers are packets only the caller can send, the same way a jump is.
MH_API int32_t mh_viewer_take_death_choice(MhViewerHandle viewer);

/// Takes the entity the player asked to talk to, if any. Returns 0 when
/// nothing is pending; the request is consumed either way.
MH_API uint32_t mh_viewer_take_talk(MhViewerHandle viewer);

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

/// Asks the player where the game is, with the platform's own folder chooser.
///
/// Needs no viewer, because it runs before there is one. On a first run there
/// is no install, so no DATs, so no glyph atlas and nothing to draw a screen
/// with - a native chooser is the only thing that works with no game data at
/// all, and SDL gives the same call on all three platforms.
///
/// **Must be called on the main thread.** macOS will not open a window of any
/// kind anywhere else, this one included.
///
/// Blocks until the player chooses or cancels, pumping events while it waits:
/// SDL reports the answer through a callback, and on a first run there is no
/// other event loop running to deliver it. Writes a NUL-terminated path into
/// `out`.
///
/// Returns 1 when a folder was chosen, 0 when cancelled, and -1 if the chooser
/// could not be opened at all.
MH_API int32_t mh_pick_folder(const char* default_location, char* out, int32_t out_size);

#ifdef __cplusplus
}
#endif

#endif // MOGHOUSE_INTEROP_H
