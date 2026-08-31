#include "moghouse_interop.h"

#include "viewer.h"

#include <cstdio>
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
            copied.push_back(std::move(entity));
        }
    }
    viewer->link.setEntities(std::move(copied));
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

} // extern "C"
