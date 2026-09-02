// The standalone viewer: read the options, run the renderer.
//
// Everything that used to live here is now in viewer.cpp behind runViewer, so
// the same code serves both this and the client that embeds it. This exists
// because being able to exercise the renderer without the client is worth
// keeping.

#include "viewer.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

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
            const int read =
                std::sscanf(one.c_str(), "%f,%f,%d,%f,%f,%23[^;]", &x, &z, &kind, &y, &heading, name);
            if (read >= 3)
            {
                options.testEntities.push_back(mh::RadarEntity{x, z, y, heading, kind, std::string{name}});
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

int main(int argc, char** argv)
{
    // A line at a time, so a log being written to a file can be read while the
    // viewer is still running. Redirected to a file, stdout is block buffered:
    // everything printed sits in the buffer until it fills or the process
    // exits, so pressing p and then going to look at the log shows nothing, and
    // the positions only appear once the window is closed. The client's own
    // path already turns buffering off for the same reason.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    return mh::runViewer(mh::optionsFromEnvironment(argc, argv));
}
