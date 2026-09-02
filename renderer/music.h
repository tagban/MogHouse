#pragma once

// Zone music.
//
// The server never names a piece of music; it sends a number, in the zone
// login reply and again whenever the situation changes, and the number is the
// file. Finding it and decoding it is ffxi::BgwStream's job; this one keeps a
// stream feeding an audio device and swaps it when the zone changes.

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace mh
{
/// One track at a time, streamed rather than held.
class Music
{
public:
    Music();
    ~Music();

    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    /// Starts a .bgw, replacing whatever is playing. An empty path stops.
    ///
    /// Asking for the track already playing does nothing, which is what makes
    /// this safe to call every time the server mentions music - it says so on
    /// every zone-in, including the one that did not change the tune.
    bool play(const std::filesystem::path& path);

    void stop();

    /// Destroys the audio stream. Idempotent, and the destructor calls it, but
    /// it has to happen *before* SDL_Quit: quitting tears down the audio
    /// subsystem, and destroying a stream afterwards locks a mutex that has
    /// already been freed. A Music whose scope ends after SDL_Quit - which is
    /// the case in runViewer - therefore has to be shut down by hand first.
    /// Getting this wrong segfaults on macOS; Windows survives it by luck.
    void shutdown();

    /// 0 silent, 1 as recorded. Applied while mixing rather than to the
    /// device, so it survives a track change.
    void setVolume(float volume);

    /// What is playing, for the caller that wants to know whether to ask.
    std::string current() const;

    /// Public because the audio callback is a free function - SDL calls C,
    /// not a member - and it has to see what it is filling.
    struct State;

private:
    std::unique_ptr<State> state_;
};
} // namespace mh
