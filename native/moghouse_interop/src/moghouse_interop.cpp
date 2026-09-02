#include "moghouse_interop.h"

#include "viewer.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace
{
/// Copies a borrowed C string, treating null as absent.
std::optional<std::string> borrow(const char* text)
{
    if (!text)
    {
        return std::nullopt;
    }
    return std::string{text};
}

std::string borrowOr(const char* text, const char* fallback)
{
    return text ? std::string{text} : std::string{fallback};
}

/// Sends this library's stdout and stderr to a file beside the app's log.
///
/// The app is a windowed executable with no console, and redirecting
/// Console.Out on the managed side does nothing for the C runtime in here - so
/// every printf the renderer makes goes to a handle that leads nowhere. Zone
/// loads, texture counts, the reason a model failed to build: all of it exists
/// and none of it is readable.
///
/// Its own file, not the app's. The managed side holds MOGHOUSE_LOG open with
/// a StreamWriter, which shares for reading only, so opening it for writing
/// here fails - and two writers on one file would tread on each other in any
/// case, because each keeps its own idea of where the end is.
///
/// The probe matters more than it looks. freopen closes the stream *before* it
/// tries to open the new target, so a failed freopen leaves stdout shut and
/// every later printf writing to a dead handle - which is exactly what stopped
/// the world window from opening at all. Nothing is redirected unless a plain
/// open has already proved it can be done.
void redirectOutputToLog()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    const char* base = std::getenv("MOGHOUSE_RENDERER_LOG");
    std::string path = base ? std::string{base} : std::string{};
    if (path.empty())
    {
        const char* appLog = std::getenv("MOGHOUSE_LOG");
        if (!appLog || !*appLog)
        {
            return;
        }
        path = std::string{appLog} + ".renderer";
    }

    if (FILE* probe = std::fopen(path.c_str(), "w"))
    {
        std::fclose(probe);
    }
    else
    {
        return;                      // cannot write there; leave stdout alone
    }

    if (std::freopen(path.c_str(), "w", stdout))
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::printf("renderer: output attached to %s\n", path.c_str());
    }
    if (std::freopen(path.c_str(), "a", stderr))
    {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}
} // namespace

/// The options are copied at create time and the link outlives every call, so
/// the caller is free to release its own strings immediately and nothing here
/// reaches back into managed memory.
struct MhViewer
{
    mh::ViewerOptions options;
    mh::ViewerLink link;
};

extern "C" {

MhViewerHandle mh_viewer_create(const MhViewerOptions* options)
{
    if (!options)
    {
        return nullptr;
    }

    redirectOutputToLog();

    auto* viewer = new MhViewer{};
    viewer->options.zonePath = borrowOr(options->zone_path, "");
    viewer->options.keyTablePath = borrowOr(options->key_table_path, "");
    viewer->options.keyTable2Path = borrowOr(options->key_table2_path, "");
    viewer->options.look = borrow(options->look);
    if (options->server_clock != 0)
    {
        viewer->options.serverClock = options->server_clock;
    }
    viewer->options.characterAt = borrow(options->character_at);
    viewer->options.characterFacing = borrow(options->character_facing);
    viewer->options.zoneName = borrow(options->zone_name);
    viewer->options.playerName = borrow(options->player_name);
    if (options->time_of_day >= 0)
    {
        viewer->options.timeOfDay = options->time_of_day;
    }
    viewer->options.screenshotPath = borrow(options->screenshot_path);
    if (options->screenshot_after_frames > 0)
    {
        viewer->options.settleFrames = options->screenshot_after_frames;
    }
    return viewer;
}

int32_t mh_viewer_run(MhViewerHandle viewer)
{
    if (!viewer)
    {
        return 2;
    }

    // Nothing may cross this boundary as an exception. A C++ throw unwinding
    // into managed code arrives as an opaque SEHException with no message,
    // which turns "the key table path was wrong" into "external component has
    // thrown an exception".
    try
    {
        return static_cast<int32_t>(mh::runViewer(viewer->options, &viewer->link));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "renderer: %s\n", error.what());
        std::fflush(stderr);
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "renderer: unknown failure\n");
        std::fflush(stderr);
        return 1;
    }
}

void mh_viewer_set_entities(MhViewerHandle viewer, const MhRadarEntity* entities, int32_t count)
{
    if (!viewer)
    {
        return;
    }

    std::vector<mh::RadarEntity> copied;
    if (entities && count > 0)
    {
        copied.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            // Bounded by the field width in case the caller filled it without
            // a terminator.
            const char* raw = entities[i].name;
            const size_t length = static_cast<size_t>(std::find(raw, raw + sizeof(entities[i].name), '\0') - raw);
            mh::RadarEntity entity{entities[i].x,    entities[i].z,
                                   entities[i].y,    entities[i].heading,
                                   entities[i].kind, std::string{raw, length},
                                   entities[i].id,   entities[i].nameHidden != 0};
            std::memcpy(entity.look, entities[i].look, sizeof(entity.look));
            entity.gmLevel = entities[i].gmLevel;
            entity.modelId = static_cast<uint16_t>(entities[i].modelId);
            entity.healthPercent = entities[i].healthPercent;
            entity.triggerable = entities[i].triggerable != 0;
            copied.push_back(std::move(entity));
        }
    }
    viewer->link.setEntities(std::move(copied));
}

void mh_viewer_set_zone_lines(MhViewerHandle viewer, const MhZoneLine* lines, int32_t count)
{
    if (!viewer)
    {
        return;
    }

    std::vector<mh::ZoneLineMarker> copied;
    if (lines && count > 0)
    {
        copied.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i)
        {
            copied.push_back(mh::ZoneLineMarker{lines[i].x, lines[i].y, lines[i].z, lines[i].radius});
        }
    }
    viewer->link.setZoneLines(std::move(copied));
}

void mh_viewer_set_death(MhViewerHandle viewer, int32_t dead, int32_t raise_offered)
{
    if (viewer)
    {
        viewer->link.setDeath(dead != 0, raise_offered != 0);
    }
}

void mh_viewer_set_vitals(MhViewerHandle viewer, uint32_t hp, uint32_t mp, uint32_t tp,
                          uint8_t hp_percent, uint8_t mp_percent)
{
    if (viewer)
    {
        viewer->link.setVitals(hp, mp, tp, hp_percent, mp_percent);
    }
}

void mh_viewer_set_music(MhViewerHandle viewer, const char* path)
{
    if (viewer)
    {
        viewer->link.setMusic(path ? std::string{path} : std::string{});
    }
}

void mh_viewer_load_zone(MhViewerHandle viewer, const char* dat_path, const char* zone_name, float x, float y,
                         float z, float heading)
{
    if (viewer && dat_path)
    {
        viewer->link.requestZone({dat_path, zone_name ? zone_name : "", x, y, z, heading});
    }
}

int32_t mh_viewer_is_loading(MhViewerHandle viewer)
{
    return viewer && viewer->link.loading() ? 1 : 0;
}

void mh_viewer_set_settings(MhViewerHandle viewer, float music_volume, int32_t radar_turns)
{
    if (viewer)
    {
        viewer->link.applySettings({music_volume, radar_turns != 0});
    }
}

int32_t mh_viewer_take_settings(MhViewerHandle viewer, float* music_volume, int32_t* radar_turns)
{
    if (!viewer || !viewer->link.settingsChanged())
    {
        return 0;
    }
    const auto settings = viewer->link.settings();
    if (music_volume)
    {
        *music_volume = settings.musicVolume;
    }
    if (radar_turns)
    {
        *radar_turns = settings.radarTurns ? 1 : 0;
    }
    return 1;
}

int32_t mh_viewer_take_link(MhViewerHandle viewer)
{
    return viewer ? static_cast<int32_t>(viewer->link.takeLink()) : 0;
}

int32_t mh_viewer_take_death_choice(MhViewerHandle viewer)
{
    return viewer ? static_cast<int32_t>(viewer->link.takeDeathChoice()) : MH_DEATH_NONE;
}

uint32_t mh_viewer_take_talk(MhViewerHandle viewer)
{
    if (!viewer)
    {
        return 0;
    }

    uint32_t entityId = 0;
    return viewer->link.takeTalk(entityId) ? entityId : 0;
}

void mh_viewer_push_chat(MhViewerHandle viewer, const char* line)
{
    if (!viewer || !line)
    {
        return;
    }
    viewer->link.pushChat(std::string{line});
}

int32_t mh_viewer_get_character(MhViewerHandle viewer, float* x, float* y, float* z, float* heading)
{
    if (!viewer || !x || !y || !z || !heading)
    {
        return 0;
    }
    return viewer->link.character(*x, *y, *z, *heading) ? 1 : 0;
}

int32_t mh_viewer_take_jump(MhViewerHandle viewer)
{
    return viewer && viewer->link.takeJump() ? 1 : 0;
}

void mh_viewer_place_character(MhViewerHandle viewer, float x, float y, float z, float heading)
{
    if (viewer)
    {
        viewer->link.placeCharacter(x, y, z, heading);
    }
}

int32_t mh_viewer_take_chat(MhViewerHandle viewer, char* buffer, int32_t capacity)
{
    if (!viewer || !buffer || capacity <= 0)
    {
        return 0;
    }

    std::optional<std::string> line = viewer->link.takeChat();
    if (!line)
    {
        return 0;
    }

    const size_t room = static_cast<size_t>(capacity) - 1;
    const size_t length = line->size() < room ? line->size() : room;
    std::memcpy(buffer, line->data(), length);
    buffer[length] = '\0';
    return 1;
}

void mh_viewer_stop(MhViewerHandle viewer)
{
    if (viewer)
    {
        viewer->link.stop();
    }
}

void mh_viewer_destroy(MhViewerHandle viewer) { delete viewer; }

namespace
{
/// What the folder chooser produced, filled in by SDL's callback.
struct FolderChoice
{
    bool done{false};
    bool chosen{false};
    std::string path;
};

void SDLCALL onFolderChosen(void* userdata, const char* const* filelist, int /*filter*/)
{
    auto* choice = static_cast<FolderChoice*>(userdata);
    if (choice == nullptr)
    {
        return;
    }

    // A null list is the chooser failing; an empty one is the player pressing
    // cancel. Both leave `chosen` false, and only the first is worth a
    // different answer to the caller - which it gets from the return value
    // below rather than from here.
    if (filelist != nullptr && filelist[0] != nullptr)
    {
        choice->path = filelist[0];
        choice->chosen = true;
    }
    choice->done = true;
}
} // namespace

int32_t mh_pick_folder(const char* default_location, char* out, int32_t out_size)
{
    if (out == nullptr || out_size <= 0)
    {
        return -1;
    }
    out[0] = '\0';

    // Video is what owns a chooser, and on a first run nothing has started it
    // yet. Init is reference counted, so asking again when the renderer is
    // already up costs nothing and does not tear anything down on the way out.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        std::printf("could not start SDL video for the folder chooser: %s\n", SDL_GetError());
        return -1;
    }

    FolderChoice choice;
    SDL_ShowOpenFolderDialog(onFolderChosen, &choice, nullptr, default_location, false);

    // SDL answers through the callback, and on a first run there is no other
    // loop to deliver it - so this one pumps until the answer arrives.
    while (!choice.done)
    {
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    if (!choice.chosen)
    {
        return 0;
    }

    if (static_cast<int32_t>(choice.path.size()) >= out_size)
    {
        std::printf("the chosen folder's path is longer than the caller's buffer\n");
        return -1;
    }

    std::memcpy(out, choice.path.c_str(), choice.path.size() + 1);
    return 1;
}

} // extern "C"
