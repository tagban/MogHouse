// C ABI boundary between the lotus-ffxi/lotus-engine C++ core and the
// MogHouse.Core C# game-logic layer. Plain C so either side can be
// consumed without a C++ toolchain.
//
// NOT YET BUILD-VERIFIED. See ../README.md for the open CMake question
// (the `ffxi` CMake target in ffxi-engine owns its module sources via a
// FILE_SET, so this header's implementation can't just target_link_libraries
// against it yet) and for the other assumptions this design makes.
#ifndef MOGHOUSE_INTEROP_H
#define MOGHOUSE_INTEROP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define PJ_API __declspec(dllexport)
#else
#define PJ_API __attribute__((visibility("default")))
#endif

typedef struct PjGame* PjGameHandle;

// Called once per frame from the engine's own main-loop thread (see
// pj_game_run below), after the engine's internal tick has run. Must return
// quickly - do real game-logic work on another thread and use this only to
// hand off timing/signals.
typedef void (*PjTickCallback)(double delta_seconds, void* user_data);

// Called if the engine hits an unhandled exception in a tick. `message` is
// only valid for the duration of the call - copy it if you need to keep it.
typedef void (*PjErrorCallback)(const char* message, void* user_data);

// Creates a game instance. Does not start the engine loop.
PJ_API PjGameHandle pj_game_create(const char* app_name, uint32_t app_version);
PJ_API void pj_game_destroy(PjGameHandle game);

PJ_API void pj_game_set_tick_callback(PjGameHandle game, PjTickCallback callback, void* user_data);
PJ_API void pj_game_set_error_callback(PjGameHandle game, PjErrorCallback callback, void* user_data);

// Blocking - runs the engine's main loop on the calling thread until
// pj_game_close() is observed. Call this from a dedicated background thread
// on the managed side; never from a UI thread.
PJ_API void pj_game_run(PjGameHandle game);

// Signals the running loop to stop after the current frame. Safe to call
// from any thread, including from within a PjTickCallback.
PJ_API void pj_game_close(PjGameHandle game);

#ifdef __cplusplus
}
#endif

#endif // MOGHOUSE_INTEROP_H
