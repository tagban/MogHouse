#pragma once

// Sound effects, several at once, over whatever music is playing.
//
// Music is one long stream that is swapped when the zone changes; an effect is
// short, sudden, and overlaps itself - a worm coming out of the ground while a
// torch crackles and somebody swings a sword. That is a different problem and
// it is why this exists beside Music rather than inside it.
//
// No mixing is done here. SDL mixes every stream bound to the same device, so
// a voice is a stream: decode the sound, push it whole, let SDL drain it, and
// throw the stream away when it runs dry. Hand-mixing would mean owning a
// callback, a clock and a clipping policy for no gain.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace mh
{
/// Short sounds, played on top of each other.
class Sounds
{
public:
    Sounds();
    ~Sounds();

    Sounds(const Sounds&) = delete;
    Sounds& operator=(const Sounds&) = delete;

    /// Plays one, at 0..1 of its own loudness. Returns false if the file could
    /// not be read or there was no voice free.
    ///
    /// Decoded once and kept: an effect is a second of audio and the same one
    /// plays over and over, so reading it off disk each time to save a hundred
    /// kilobytes is the wrong trade.
    bool play(const std::filesystem::path& path, float volume = 1.0f);

    /// Reaps voices that have finished. Cheap, and wants calling once a frame -
    /// without it the finished streams pile up until the device runs out.
    void tick();

    /// How many are sounding now, for the log and for tests.
    size_t voices() const;

    /// Destroys every stream. Has to happen *before* SDL_Quit for the same
    /// reason Music::shutdown does: quitting tears down the audio subsystem,
    /// and destroying a stream afterwards locks a mutex that has been freed.
    void shutdown();

private:
    /// More than this sounding at once and the newest is dropped. A number
    /// rather than no limit because a bug that plays a sound every frame
    /// should be quiet and obvious, not a thousand voices and a stall.
    static constexpr size_t kMaxVoices = 24;

    struct Decoded
    {
        std::vector<int16_t> samples;
        uint32_t sampleRate{48000};
        int channels{1};
    };

    const Decoded* decode(const std::filesystem::path& path);

    mutable std::mutex mutex_;
    std::map<std::string, Decoded> known_;
    std::vector<SDL_AudioStream*> playing_;
    bool stopped_{false};
};
} // namespace mh
