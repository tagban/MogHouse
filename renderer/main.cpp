// The standalone viewer: read the options, run the renderer.
//
// Everything that used to live here is now in viewer.cpp behind runViewer, so
// the same code serves both this and the client that embeds it. This exists
// because being able to exercise the renderer without the client is worth
// keeping.

#include "viewer.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
/// An environment variable, or nothing if it is not set. Several of these mean
/// something by their presence alone, so absent and empty are not the same.
std::optional<std::string> fromEnvironment(const char* name)
{
    const char* value = std::getenv(name);
    if (!value)
    {
        return std::nullopt;
    }
    return std::string{value};
}

std::string fromEnvironmentOr(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{fallback};
}
} // namespace

namespace mh
{
ViewerOptions optionsFromEnvironment(int argc, char** argv)
{
    ViewerOptions options;

    options.zonePath = argc > 1 ? argv[1] : "";
    options.keyTablePath = fromEnvironmentOr("MOGHOUSE_FFXI_KEYTABLE", "");
    options.keyTable2Path = fromEnvironmentOr("MOGHOUSE_FFXI_KEYTABLE2", "");

    options.look = fromEnvironment("MOGHOUSE_LOOK");
    options.characterPath = fromEnvironment("MOGHOUSE_CHARACTER");
    options.characterAt = fromEnvironment("MOGHOUSE_CHARACTER_AT");
    options.characterFacing = fromEnvironment("MOGHOUSE_CHARACTER_FACING");
    options.camera = fromEnvironment("MOGHOUSE_CAMERA");
    options.cameraLook = fromEnvironment("MOGHOUSE_CAMERA_LOOK");
    options.animation = fromEnvironment("MOGHOUSE_ANIMATION");
    options.zoneName = fromEnvironment("MOGHOUSE_ZONE_NAME");

    // Semicolon separated, oldest first - enough to frame the chat panel
    // without standing up a server session.
    if (const std::optional<std::string> chat = fromEnvironment("MOGHOUSE_CHAT"))
    {
        size_t at = 0;
        while (at <= chat->size())
        {
            const size_t end = chat->find(';', at);
            options.testChat.push_back(chat->substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos)
            {
                break;
            }
            at = end + 1;
        }
    }
    options.screenshotPath = fromEnvironment("MOGHOUSE_SCREENSHOT");
    options.mapPath = fromEnvironment("MOGHOUSE_MAP");

    if (const std::optional<std::string> settle = fromEnvironment("MOGHOUSE_SCREENSHOT_AFTER"))
    {
        options.settleFrames = std::atoi(settle->c_str());
    }

    if (const std::optional<std::string> entities = fromEnvironment("MOGHOUSE_ENTITIES"))
    {
        size_t at = 0;
        while (at < entities->size())
        {
            const size_t end = entities->find(';', at);
            const std::string one = entities->substr(at, end == std::string::npos ? std::string::npos : end - at);
            float x = 0.0f;
            float z = 0.0f;
            int kind = 0;
            float y = 0.0f;
            float heading = 0.0f;
            // "x,z,kind" still works; "x,z,kind,y,heading" stands a body
            // there too. The short form leaves it at ground level facing +z,
            // which is what a radar-only test wants.
            char name[24] = {};
            unsigned model = 0;
            // "x,z,kind" still works, "x,z,kind,y,heading" stands a body, and
            // a sixth field is a model id - which is the only way to watch a
            // worm come out of the ground without a server that has one.
            const int read = std::sscanf(one.c_str(), "%f,%f,%d,%f,%f,%u,%23[^;]", &x, &z, &kind, &y,
                                         &heading, &model, name);
            if (read == 6 || read < 6)
            {
                // The name is the sixth field in the old form and the seventh
                // in the new one, so a line without a model is read again.
                if (read < 6)
                {
                    std::sscanf(one.c_str(), "%f,%f,%d,%f,%f,%23[^;]", &x, &z, &kind, &y, &heading, name);
                    model = 0;
                }
            }
            if (read >= 3)
            {
                mh::RadarEntity made{x, z, y, heading, kind, std::string{name}};
                made.modelId = static_cast<uint16_t>(model);

                // Counted as arriving the moment the window opens, so the
                // spawn effect actually plays. Without this they are simply
                // there, which is what every other test entity wants.
                made.spawnedSecondsAgo = 0.0f;
                options.testEntities.push_back(std::move(made));
            }
            if (end == std::string::npos)
            {
                break;
            }
            at = end + 1;
        }
    }

    // 1 lays the character out dead, 2 offers them a raise as well. The box
    // is otherwise reachable only by dying on a real server.
    if (const std::optional<std::string> death = fromEnvironment("MOGHOUSE_DEAD"))
    {
        options.testDeath = std::atoi(death->c_str());
    }

    // MOGHOUSE_FORM=1 shows a stand-in login screen, for looking at the form
    // widget without standing up a client to set one.
    if (const std::optional<std::string> form = fromEnvironment("MOGHOUSE_FORM"))
    {
        options.testForm = std::atoi(form->c_str());
    }

    if (const std::optional<std::string> frame = fromEnvironment("MOGHOUSE_FRAME"))
    {
        options.frame = static_cast<float>(std::atof(frame->c_str()));
    }
    if (const std::optional<std::string> time = fromEnvironment("MOGHOUSE_TIME"))
    {
        options.timeOfDay = std::atoi(time->c_str());
    }
    if (const std::optional<std::string> sequence = fromEnvironment("MOGHOUSE_SCREENSHOT_SEQUENCE"))
    {
        options.screenshotSequence = std::atoi(sequence->c_str());
    }
    if (const std::optional<std::string> cutout = fromEnvironment("MOGHOUSE_CUTOUT"))
    {
        options.cutoutMode = std::atoi(cutout->c_str());
    }
    if (const std::optional<std::string> mode = fromEnvironment("MOGHOUSE_SHADER_MODE"))
    {
        options.shaderMode = static_cast<float>(std::atof(mode->c_str()));
    }
    return options;
}
} // namespace mh

namespace
{
/// A set of bags with nothing behind them, for looking at the panel without a
/// server.
///
/// The client is what normally fills these - it reads the item DATs, since it
/// already has the file table open, and pushes a name and an icon per distinct
/// item. That leaves the panel itself untestable except by logging in, which
/// is a slow way to find out a column is one slot too wide.
///
/// The icons here are generated rather than real: a flat colour with a lighter
/// diagonal across it, which is enough to see that a cell landed in the right
/// place and the right way up. A diagonal catches what a plain square cannot,
/// because the pixels are flipped twice between the DAT and the screen and two
/// flips look exactly like none.
void fillDemoBags(mh::ViewerLink& link)
{
    static const uint16_t kSizes[18] = {30, 0, 0, 0, 0, 20, 0, 0, 8, 0, 8, 0, 0, 0, 0, 0, 0, 0};

    std::vector<mh::ViewerLink::InventorySlot> slots;
    const uint16_t kItems[] = {4096, 4097, 12579, 13952, 17440, 4381, 640, 5, 100, 2, 17441, 12568};
    for (int i = 0; i < 24; ++i)
    {
        const uint16_t item = kItems[i % (sizeof(kItems) / sizeof(kItems[0]))];
        slots.push_back(mh::ViewerLink::InventorySlot{
            0, static_cast<uint8_t>(i), item, static_cast<uint32_t>(i % 4 == 0 ? 1 : (i * 7) % 99 + 1)});
    }
    for (int i = 0; i < 5; ++i)
    {
        slots.push_back(mh::ViewerLink::InventorySlot{
            5, static_cast<uint8_t>(i * 3), kItems[i], static_cast<uint32_t>(i + 1)});
    }

    // Slot zero of the inventory is the gil, as it is on a real server: item
    // 65535, which no item DAT holds. Here so the panel is exercised against
    // the thing that looked like a bug - a count with nothing under it.
    slots.push_back(mh::ViewerLink::InventorySlot{0, 0, 65535, 4821});

    link.setInventory(slots.data(), static_cast<int>(slots.size()), kSizes, 18);

    static const char* kNames[] = {"Ice Crystal", "Wind Crystal", "Scorpion Harness", "Ochiudo's Kote",
                                   "Kraken Club", "Meat Mithkabob", "Bronze Sword",   "Chocobo Bedding",
                                   "Okadomatsu",  "Simple Bed",     "Kraken Club +1", "Leather Vest"};
    for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); ++i)
    {
        mh::ViewerLink::ItemFace face;
        face.itemId = kItems[i];
        face.name = kNames[i];
        face.description = "A stand-in, drawn without reading any file.\n"
                           "It has two lines so the tooltip has to wrap.\n"
                           "DEF:12 Ice-20 Dark+15";

        // Spread across the kinds and levels so the sort headings have
        // something to tell apart.
        face.type = static_cast<uint16_t>((i % 4) + 4);
        face.level = static_cast<uint16_t>((i * 7) % 75);

        // Every third one is wearable, so the right click menu is exercised
        // both with an Equip on it and without.
        face.slots = i % 3 == 0 ? static_cast<uint16_t>(1 << (i % 16)) : 0;
        face.width = 32;
        face.height = 32;
        face.rgba.resize(32 * 32 * 4);

        const float hue = static_cast<float>(i) / 12.0f * 6.0f;
        const int sector = static_cast<int>(hue) % 6;
        const float fade = hue - static_cast<float>(static_cast<int>(hue));
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        switch (sector)
        {
        case 0: rgb[0] = 1.0f; rgb[1] = fade; break;
        case 1: rgb[0] = 1.0f - fade; rgb[1] = 1.0f; break;
        case 2: rgb[1] = 1.0f; rgb[2] = fade; break;
        case 3: rgb[1] = 1.0f - fade; rgb[2] = 1.0f; break;
        case 4: rgb[2] = 1.0f; rgb[0] = fade; break;
        default: rgb[2] = 1.0f - fade; rgb[0] = 1.0f; break;
        }

        for (int y = 0; y < 32; ++y)
        {
            for (int x = 0; x < 32; ++x)
            {
                // Top left is brightest, and the corner rounds off, so both
                // the orientation and the cell bounds are visible.
                const bool onDiagonal = std::abs((31 - y) - x) < 4;
                const float lift = onDiagonal ? 1.0f : 0.55f;
                const size_t at = (static_cast<size_t>(y) * 32 + x) * 4;
                face.rgba[at + 0] = static_cast<uint8_t>(rgb[0] * 255.0f * lift);
                face.rgba[at + 1] = static_cast<uint8_t>(rgb[1] * 255.0f * lift);
                face.rgba[at + 2] = static_cast<uint8_t>(rgb[2] * 255.0f * lift);
                face.rgba[at + 3] = (x + y < 4 || x + y > 58) ? 0 : 255;
            }
        }

        link.pushItemFace(std::move(face));
    }
}
} // namespace

int main(int argc, char** argv)
{
    // A line at a time, so a log being written to a file can be read while the
    // viewer is still running. Redirected to a file, stdout is block buffered:
    // everything printed sits in the buffer until it fills or the process
    // exits, so pressing p and then going to look at the log shows nothing, and
    // the positions only appear once the window is closed. The client's own
    // path already turns buffering off for the same reason.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (fromEnvironment("MOGHOUSE_INVENTORY_DEMO"))
    {
        static mh::ViewerLink demo;
        fillDemoBags(demo);
        return mh::runViewer(mh::optionsFromEnvironment(argc, argv), &demo);
    }

    return mh::runViewer(mh::optionsFromEnvironment(argc, argv));
}
