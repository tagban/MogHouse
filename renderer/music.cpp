#include "music.h"

#include "ffxi/bgw.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace mh
{
struct Music::State
{
    SDL_AudioStream* stream{};
    ffxi::BgwStream source;
    std::string playing;
    float volume{0.35f};

    /// Guards `source` and `playing`, which the audio thread reads and the
    /// game thread writes when the zone changes.
    mutable std::mutex lock;

    /// Scratch for one callback's worth of samples. Kept rather than allocated
    /// per call: this runs on the audio thread, where a malloc that stalls is
    /// a gap in the music.
    std::vector<int16_t> scratch;
};

namespace
{
/// SDL asks for more; we decode that much and hand it over.
void SDLCALL feed(void* userData, SDL_AudioStream* stream, int additional, int)
{
    auto* state = static_cast<Music::State*>(userData);
    if (additional <= 0)
    {
        return;
    }

    const size_t frames = static_cast<size_t>(additional) / (sizeof(int16_t) * ffxi::BgwStream::kChannels);
    if (frames == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> held{state->lock};
    if (state->playing.empty())
    {
        return;
    }

    state->scratch.assign(frames * ffxi::BgwStream::kChannels, 0);
    const size_t got = state->source.read(state->scratch.data(), frames);
    if (got == 0)
    {
        // The track ended and does not loop. Say so, so nothing keeps asking.
        state->playing.clear();
        return;
    }

    if (state->volume < 0.999f)
    {
        for (size_t i = 0; i < got * ffxi::BgwStream::kChannels; ++i)
        {
            state->scratch[i] = static_cast<int16_t>(static_cast<float>(state->scratch[i]) * state->volume);
        }
    }

    SDL_PutAudioStreamData(stream, state->scratch.data(),
                           static_cast<int>(got * ffxi::BgwStream::kChannels * sizeof(int16_t)));
}
} // namespace

Music::Music()
    : state_(std::make_unique<State>())
{
    // Audio is opened beside video rather than instead of it: a client that
    // cannot make a sound should still draw, so a failure here is reported and
    // then ignored.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::printf("no audio: %s\n", SDL_GetError());
        return;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = ffxi::BgwStream::kChannels;
    spec.freq = static_cast<int>(ffxi::BgwStream::kDefaultSampleRate);

    state_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, state_.get());
    if (!state_->stream)
    {
        std::printf("no audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(state_->stream);
}

void Music::shutdown()
{
    if (state_ && state_->stream)
    {
        SDL_DestroyAudioStream(state_->stream);
        state_->stream = nullptr;
    }
}

Music::~Music()
{
    shutdown();
}

bool Music::play(const std::filesystem::path& path)
{
    if (!state_ || !state_->stream)
    {
        return false;
    }

    const std::string wanted = path.string();
    std::lock_guard<std::mutex> held{state_->lock};

    // The server mentions music on every zone-in, including the ones that did
    // not change it. Restarting the same track each time would make walking
    // between two rooms of one zone sound like a stutter.
    if (wanted == state_->playing)
    {
        return true;
    }

    if (wanted.empty())
    {
        state_->playing.clear();
        return true;
    }

    if (!state_->source.open(path))
    {
        std::printf("music: %s is not a BGW\n", wanted.c_str());
        state_->playing.clear();
        return false;
    }

    state_->playing = wanted;

    // Tell the stream what rate this track is, rather than assuming the one the
    // device was opened at. Twenty-nine of the hundred and eleven tracks in a
    // retail install are 48000 and the rest are 44100; played at a flat 44100
    // those twenty-nine run about nine per cent slow, which sounds like a
    // slightly flat recording rather than like a bug, and so went unnoticed.
    SDL_AudioSpec source{};
    source.format = SDL_AUDIO_S16LE;
    source.channels = ffxi::BgwStream::kChannels;
    source.freq = static_cast<int>(state_->source.sampleRate());
    if (!SDL_SetAudioStreamFormat(state_->stream, &source, nullptr))
    {
        std::printf("music: could not set %u Hz: %s\n", state_->source.sampleRate(), SDL_GetError());
    }

    std::printf("music: track %u, %u Hz%s\n", state_->source.track(), state_->source.sampleRate(),
                state_->source.loops() ? ", looping" : ", once");
    return true;
}

void Music::stop()
{
    if (state_)
    {
        std::lock_guard<std::mutex> held{state_->lock};
        state_->playing.clear();
    }
}

void Music::setVolume(float volume)
{
    if (state_)
    {
        std::lock_guard<std::mutex> held{state_->lock};
        state_->volume = std::clamp(volume, 0.0f, 1.0f);
    }
}

std::string Music::current() const
{
    if (!state_)
    {
        return {};
    }
    std::lock_guard<std::mutex> held{state_->lock};
    return state_->playing;
}
} // namespace mh
