#pragma once

// The renderer as something you can call, rather than something you run.
//
// MogHouse ships as one application, so the renderer has to be a library the
// C# client loads rather than a second executable it talks to over a socket. A
// socket would have meant designing, versioning and debugging a wire format
// between two halves of the same program, and then throwing it away.
//
// The standalone `moghouse-renderer` still exists and is a thin wrapper over
// this - it is how the renderer gets exercised without the client, which is
// worth keeping.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mh
{
/// One thing on the radar. Positions are world x and z; height is not shown,
/// because a radar is a plan view and a dot above you is still a dot.
struct RadarEntity
{
    float x{};
    float z{};

    /// 0 player, 1 npc, 2 enemy - matching MogHouse.Core's FfxiEntityKind.
    int kind{};
};

/// Everything the viewer needs to start. Fields that were environment
/// variables keep their meaning; an unset optional means the variable was
/// absent, which for several of them is different from being empty.
struct ViewerOptions
{
    /// The zone DAT to open. Empty opens a window with nothing in it, which is
    /// how the graphics path gets checked on a machine with no game installed.
    std::string zonePath;

    /// The two 256-byte tables MZB and MMB decryption need.
    std::string keyTablePath;
    std::string keyTable2Path;

    /// A player character as race,face,head,body,hands,legs,feet.
    std::optional<std::string> look;

    /// An NPC that lives in one DAT, as semicolon-separated paths. Ignored
    /// when `look` is set.
    std::optional<std::string> characterPath;

    std::optional<std::string> characterAt;      ///< "x,y,z"
    std::optional<std::string> characterFacing;  ///< compass degrees, 0 is north (+z)
    std::optional<std::string> camera;           ///< "x,y,z"
    std::optional<std::string> cameraLook;       ///< "yaw,pitch" in degrees

    /// Pins one animation. Unset lets movement choose idle, walk or run.
    std::optional<std::string> animation;

    /// Pins the animation clock to a frame, so a screenshot is repeatable.
    std::optional<float> frame;

    /// Pins the Vana'diel clock to hhmm. Unset lets the day run.
    std::optional<int> timeOfDay;

    /// Writes a frame and quits. With `screenshotSequence` set, the path is
    /// treated as a printf format and one file is written per source frame.
    std::optional<std::string> screenshotPath;
    int screenshotSequence{};

    /// Entities to show on the radar before anything is connected, as
    /// "x,z,kind;x,z,kind". The radar is most of the way to useless without
    /// something on it, and this is how it gets checked without a server.
    std::vector<RadarEntity> testEntities;

    /// Writes the baked top-down map out as a BMP, for looking at directly.
    std::optional<std::string> mapPath;

    /// 0 never cuts out, 1 always, anything else lets each texture decide.
    int cutoutMode{2};

    /// 0 draws colour with no alpha discard, 2 draws alpha as greyscale.
    float shaderMode{};
};

/// Reads the options the standalone viewer has always taken: the zone from
/// argv[1] and everything else from MOGHOUSE_* in the environment.
ViewerOptions optionsFromEnvironment(int argc, char** argv);

/// Opens a window and runs until it closes. Returns a process exit code.
///
/// Blocking, and it owns the window and the event loop while it runs - the
/// same shape the engine this replaced had. A caller that needs to keep doing
/// its own work runs this on its own thread.
int runViewer(const ViewerOptions& options);
} // namespace mh
