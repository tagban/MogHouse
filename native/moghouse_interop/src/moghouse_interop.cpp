// Implementation of the C ABI declared in moghouse_interop.h.
//
// NOT YET BUILD-VERIFIED - there is no CMake/Ninja/Vulkan SDK toolchain in
// this environment yet, so nothing below has been compiled. It is written
// against the real lotus::Game / lotus::Engine / FFXIGame signatures (read
// directly from engine/lotus/game.cppm, engine/lotus/engine.cppm and
// ffxi-engine/ffxi/game.cppm on 2026-08-27), not guessed, but treat it as a
// design draft until it's actually built. See ../README.md for the known
// open questions (module/CMake target linkage in particular).

import ffxi;
import lotus;

#include "moghouse_interop.h"

#include <chrono>
#include <string>

namespace
{
// Subclasses FFXIGame (not lotus::Game directly) so we keep lotus-ffxi's
// existing DAT/zone/model loading in `entry()` for free, and only add a
// callback hook on top of its `tick()`.
class MogHouseGame : public FFXIGame
{
public:
    explicit MogHouseGame(const lotus::Settings& settings) : FFXIGame(settings) {}

    PjTickCallback tick_callback{nullptr};
    void* tick_user_data{nullptr};
    PjErrorCallback error_callback{nullptr};
    void* error_user_data{nullptr};

protected:
    // lotus::Engine's main loop coroutine calls this once per frame via
    // Game::tick_all (see engine/lotus/game.cppm). We forward to FFXIGame's
    // own tick first so zone/entity simulation still happens, then notify
    // the managed side.
    lotus::Task<> tick(lotus::time_point time, lotus::duration delta) override
    {
        co_await FFXIGame::tick(time, delta);

        if (tick_callback)
        {
            const double delta_seconds = std::chrono::duration<double>(delta).count();
            tick_callback(delta_seconds, tick_user_data);
        }
    }
};
} // namespace

struct PjGame
{
    lotus::Settings settings;
    MogHouseGame game;

    PjGame(std::string app_name, uint32_t app_version) : settings(make_settings(std::move(app_name), app_version)), game(settings) {}

private:
    static lotus::Settings make_settings(std::string app_name, uint32_t app_version)
    {
        lotus::Settings s;
        s.app_name = std::move(app_name);
        s.app_version = app_version;
        return s;
    }
};

extern "C"
{
    PJ_API PjGameHandle pj_game_create(const char* app_name, uint32_t app_version)
    {
        return new PjGame(app_name ? std::string(app_name) : std::string("MogHouse"), app_version);
    }

    PJ_API void pj_game_destroy(PjGameHandle game)
    {
        delete game;
    }

    PJ_API void pj_game_set_tick_callback(PjGameHandle game, PjTickCallback callback, void* user_data)
    {
        game->game.tick_callback = callback;
        game->game.tick_user_data = user_data;
    }

    PJ_API void pj_game_set_error_callback(PjGameHandle game, PjErrorCallback callback, void* user_data)
    {
        game->game.error_callback = callback;
        game->game.error_user_data = user_data;
    }

    PJ_API void pj_game_run(PjGameHandle game)
    {
        // FFXIGame::run() (inherited from lotus::Game) blocks until
        // engine->close() is called - see engine/lotus/engine.cppm. Any
        // unhandled exception here is not yet routed to error_callback;
        // that's the one real gap in this draft, left as a TODO because
        // lotus::Engine::run()'s exception-handling behavior wasn't
        // confirmed by reading the .cpp (only the .cppm interface was
        // available to inspect).
        game->game.run();
    }

    PJ_API void pj_game_close(PjGameHandle game)
    {
        game->game.engine->close();
    }
}
