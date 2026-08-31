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

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mh
{
/// One tracked thing. The radar only needs x and z - it is a plan view, and a
/// dot above you is still a dot - but the same list now also puts a body in
/// the world, which needs somewhere to stand and a way to face.
struct RadarEntity
{
    float x{};
    float z{};

    /// World height, Y up. The radar ignores it.
    float y{};

    /// Compass heading in radians, 0 along +z, the same convention the player
    /// character and the radar notch use.
    float heading{};

    /// 0 player, 1 npc, 2 enemy - matching MogHouse.Core's FfxiEntityKind.
    int kind{};

    /// Shown over the body. Empty draws nothing, which is what an entity the
    /// server has not named yet should look like.
    std::string name;

    /// The server's id for this entity, 0x1000000 | zone << 12 | targid.
    ///
    /// Carried so a name can be found for it. The server sends NPCs with no
    /// name at all - the names are in the client's own files, one table per
    /// zone - so without this every NPC in a city is anonymous.
    uint32_t id{};

    /// The server wants the name shown only when this is targeted. Doors and
    /// zone lines are named in the table but not labelled on screen.
    bool nameHidden{};

    /// Race, face, head, body, hands, legs, feet - what to build this one out
    /// of, when the server describes it the way it describes a player. All
    /// zero means it does not, and the shared body stands in.
    uint16_t look[7]{};

    /// GM level, 0 for an ordinary player.
    int gmLevel{};

    /// Whether there is a look here worth building. Race zero is not a race.
    bool hasLook() const { return look[0] != 0; }
};

/// How many tracked entities get drawn as bodies. Beyond this they stay dots -
/// they all share one skinned mesh, so the cost is per instance and small, but
/// a crowded city zone should not be able to grow the buffer without limit.
inline constexpr int kMaxDrawnBodies = 48;

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

    /// Shown along the bottom of the radar. The renderer has no zone-name
    /// table and does not want one - the client already knows what zone it
    /// asked for, so it says.
    std::optional<std::string> zoneName;

    /// Our own character's name, for the plate over its head. Unset draws none,
    /// which is what the standalone viewer with nobody logged in wants.
    std::optional<std::string> playerName;
    std::optional<std::string> camera;           ///< "x,y,z"
    std::optional<std::string> cameraLook;       ///< "yaw,pitch" in degrees

    /// Pins one animation. Unset lets movement choose idle, walk or run.
    std::optional<std::string> animation;

    /// Pins the animation clock to a frame, so a screenshot is repeatable.
    std::optional<float> frame;

    /// Pins the Vana'diel clock to hhmm. Unset lets the day run.
    std::optional<int> timeOfDay;

    /// The server's Vana'diel clock at zone-in, in Vana'diel seconds.
    ///
    /// Without it the renderer runs its own day at a made-up rate, and two
    /// clients side by side show different light and different weather, which
    /// is exactly when you most want them to agree. Vana'diel runs 25 times
    /// real time - one of its minutes is 2.4 seconds - so this is seeded once
    /// and advanced from there.
    std::optional<uint32_t> serverClock;

    /// Writes a frame and quits. With `screenshotSequence` set, the path is
    /// treated as a printf format and one file is written per source frame.
    std::optional<std::string> screenshotPath;
    int screenshotSequence{};

    /// Entities to show on the radar before anything is connected, as
    /// "x,z,kind;x,z,kind". The radar is most of the way to useless without
    /// something on it, and this is how it gets checked without a server.
    std::vector<RadarEntity> testEntities;

    /// Chat lines for a run with no client attached, so the panel can be
    /// framed and checked without a server session.
    std::vector<std::string> testChat;

    /// How many frames to let pass before taking a screenshot. The default
    /// is just enough to let the first frames settle; a caller feeding the
    /// viewer from outside wants longer, because a shot taken before anything
    /// has been posted shows an empty radar and proves nothing.
    int settleFrames{5};

    /// Writes the baked top-down map out as a BMP, for looking at directly.
    std::optional<std::string> mapPath;

    /// 0 never cuts out, 1 always, anything else lets each texture decide.
    int cutoutMode{2};

    /// 0 draws colour with no alpha discard, 2 draws alpha as greyscale.
    float shaderMode{};
};

/// The live half of a running viewer: what a caller on another thread can
/// change while it runs, and how to ask it to stop.
///
/// Deliberately tiny. Everything the client needs to say to the renderer today
/// is "here is what is nearby" and "please close", and a narrow surface is what
/// makes the C ABI over it worth having rather than a chore to maintain.
///
/// The viewer owns the window and the event loop on whatever thread calls
/// runViewer, so every method here is safe to call from another one.
class ViewerLink
{
public:
    /// Replaces the radar contents. Wholesale rather than merged: the tracker
    /// on the other side already answers "what can be seen right now", and
    /// merging here would mean two places deciding when something has gone.
    void setEntities(std::vector<RadarEntity> entities);

    /// A copy, so the render loop is never holding the lock while it draws.
    std::vector<RadarEntity> entities() const;

    /// Asks the viewer to close its window and return.
    void stop();
    bool stopping() const;

    /// Adds a line to the chat panel, dropping the oldest.
    ///
    /// The renderer never asks what a line means - colour, sender and channel
    /// are the client's business. This is a window onto whether anything is
    /// arriving at all.
    void pushChat(const std::string& line);
    std::vector<std::string> chat() const;

    /// Where the character has walked to, posted every frame.
    ///
    /// The client needs this because it, not the renderer, talks to the
    /// server: without it a character walks around on screen while standing
    /// still in the world, which is exactly what it looks like - present,
    /// named, and not moving.
    ///
    /// Y is up here, as everywhere past the DAT readers. The caller converts.
    void setCharacter(float x, float y, float z, float heading);
    bool character(float& x, float& y, float& z, float& heading) const;

    /// A jump the player asked for, consumed by whoever reads it.
    ///
    /// The renderer plays the animation locally the moment the key is pressed,
    /// but only the client talks to the server, and a jump nobody else is told
    /// about is a jump only we can see. FFXI has a packet of its own for this
    /// - it is not an emote - so this is the signal that one is owed.
    void requestJump();
    bool takeJump();

    /// A line the player typed and pressed return on, taken once.
    ///
    /// The renderer has no socket; the client does. Anything typed here has to
    /// cross over to be said, including the GM commands starting with '!' that
    /// are how a character moves between zones for now.
    void submitChat(const std::string& line);
    std::optional<std::string> takeChat();

private:
    mutable std::mutex mutex_;
    std::vector<RadarEntity> entities_;
    std::atomic<bool> stop_{false};
    float character_[4]{};
    bool haveCharacter_{false};
    std::atomic<bool> jump_{false};
    std::deque<std::string> outgoing_;
    std::deque<std::string> chat_;
};

/// Reads the options the standalone viewer has always taken: the zone from
/// argv[1] and everything else from MOGHOUSE_* in the environment.
ViewerOptions optionsFromEnvironment(int argc, char** argv);

/// Opens a window and runs until it closes. Returns a process exit code.
///
/// Blocking, and it owns the window and the event loop while it runs - the
/// same shape the engine this replaced had. A caller that needs to keep doing
/// its own work runs this on its own thread.
///
/// `link`, if given, is how that caller feeds it afterwards.
int runViewer(const ViewerOptions& options, ViewerLink* link = nullptr);
} // namespace mh
