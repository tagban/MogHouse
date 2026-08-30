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

/// One thing on the radar, in world coordinates. Height is not carried: the
/// radar is a plan view, and a dot above you is still a dot.
typedef struct MhRadarEntity
{
    float x;
    float z;
    int32_t kind;
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

    /// "x,y,z" to stand the character at, or null to pick somewhere.
    const char* character_at;

    /// Compass degrees, or null.
    const char* character_facing;

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

/// Asks a running viewer to close. mh_viewer_run returns shortly after.
MH_API void mh_viewer_stop(MhViewerHandle viewer);

/// Frees the viewer. Stop it and let mh_viewer_run return first.
MH_API void mh_viewer_destroy(MhViewerHandle viewer);

#ifdef __cplusplus
}
#endif

#endif // MOGHOUSE_INTEROP_H
