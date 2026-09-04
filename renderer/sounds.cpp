#include "sounds.h"

#include "ffxi/spw.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace mh
{
Sounds::Sounds()
{
    // Brought up here rather than assumed. Music does the same in its own
    // constructor, and either may be built first - this one is declared early
    // because the code that plays a sound sits above the music in the frame
    // function. Initialising a subsystem twice is counted rather than refused,
    // so both asking is correct.
    //
    // A client that cannot make a sound should still draw, so a failure is
    // reported and then ignored.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::printf("no audio for sound effects: %s\n", SDL_GetError());
        stopped_ = true;
    }
}

Sounds::~Sounds()
{
    shutdown();
}

const Sounds::Decoded* Sounds::decode(const std::filesystem::path& path)
{
    const std::string key = path.string();
    if (auto found = known_.find(key); found != known_.end())
    {
        return found->second.samples.empty() ? nullptr : &found->second;
    }

    // A file that will not decode is remembered as empty rather than retried.
    // Something asking for a sound that does not exist will ask again next
    // frame, and reading a bad file sixty times a second is worse than
    // remembering that it is bad.
    Decoded& into = known_[key];

    if (const std::optional<ffxi::SpwSound> sound = ffxi::loadSpw(path))
    {
        into.samples = sound->samples;
        into.sampleRate = sound->sampleRate;
        into.channels = sound->channels;
        return &into;
    }

    std::printf("sound: could not read %s\n", key.c_str());
    return nullptr;
}

bool Sounds::play(const std::filesystem::path& path, float volume)
{
    std::lock_guard<std::mutex> held{mutex_};
    if (stopped_)
    {
        return false;
    }

    const Decoded* sound = decode(path);
    if (sound == nullptr)
    {
        return false;
    }

    if (playing_.size() >= kMaxVoices)
    {
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = sound->channels;
    spec.freq = static_cast<int>(sound->sampleRate);

    // Its own stream on the default device. SDL mixes everything bound to one
    // device, so this is the whole mixer - and it resamples too, which matters
    // because the effects are 48000 and most of the music is 44100.
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
    {
        std::printf("sound: no audio stream for %s: %s\n", path.string().c_str(), SDL_GetError());
        return false;
    }

    SDL_SetAudioStreamGain(stream, std::clamp(volume, 0.0f, 1.0f));

    // Pushed whole. These are a second or two, so there is nothing to be
    // gained by feeding them in pieces and a callback to get wrong if we did.
    if (!SDL_PutAudioStreamData(stream, sound->samples.data(),
                                static_cast<int>(sound->samples.size() * sizeof(int16_t))))
    {
        std::printf("sound: could not queue %s: %s\n", path.string().c_str(), SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return false;
    }

    SDL_FlushAudioStream(stream);
    SDL_ResumeAudioStreamDevice(stream);
    playing_.push_back(stream);
    return true;
}

void Sounds::tick()
{
    std::lock_guard<std::mutex> held{mutex_};
    if (stopped_)
    {
        return;
    }

    // Drained means done. Nothing else says so: a stream that has played
    // everything given to it simply has nothing available, and left alone they
    // accumulate until the device refuses to open another.
    std::erase_if(playing_, [](SDL_AudioStream* stream) {
        if (SDL_GetAudioStreamAvailable(stream) > 0)
        {
            return false;
        }
        SDL_DestroyAudioStream(stream);
        return true;
    });
}

size_t Sounds::voices() const
{
    std::lock_guard<std::mutex> held{mutex_};
    return playing_.size();
}

void Sounds::shutdown()
{
    std::lock_guard<std::mutex> held{mutex_};
    for (SDL_AudioStream* stream : playing_)
    {
        SDL_DestroyAudioStream(stream);
    }
    playing_.clear();
    stopped_ = true;
}
} // namespace mh
