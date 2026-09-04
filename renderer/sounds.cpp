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
        into.loops = sound->loopFrame.has_value();
        into.loopSample = into.loops ? *sound->loopFrame * static_cast<size_t>(sound->channels) : 0;
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

uint32_t Sounds::hold(const std::filesystem::path& path, float volume)
{
    std::lock_guard<std::mutex> held{mutex_};
    if (stopped_)
    {
        return 0;
    }

    const Decoded* sound = decode(path);
    if (sound == nullptr || !sound->loops)
    {
        return 0;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = sound->channels;
    spec.freq = static_cast<int>(sound->sampleRate);

    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
    {
        std::printf("sound: no audio stream for %s: %s\n", path.string().c_str(), SDL_GetError());
        return 0;
    }

    SDL_SetAudioStreamGain(stream, std::clamp(volume, 0.0f, 1.0f));

    // The first pass is the whole sound; every pass after it starts at the
    // loop point, which is why refill is not used for this one.
    const Held voice{stream, sound};
    if (!SDL_PutAudioStreamData(stream, sound->samples.data(),
                                static_cast<int>(sound->samples.size() * sizeof(int16_t))))
    {
        std::printf("sound: could not queue %s: %s\n", path.string().c_str(), SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return 0;
    }
    SDL_ResumeAudioStreamDevice(stream);

    const uint32_t handle = nextHandle_++;
    holding_[handle] = voice;
    return handle;
}

int Sounds::channels(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> held{mutex_};
    if (stopped_)
    {
        return 0;
    }
    const Decoded* sound = decode(path);
    return sound != nullptr ? sound->channels : 0;
}

void Sounds::setVolume(uint32_t handle, float volume)
{
    std::lock_guard<std::mutex> held{mutex_};
    if (auto found = holding_.find(handle); found != holding_.end())
    {
        SDL_SetAudioStreamGain(found->second.stream, std::clamp(volume, 0.0f, 1.0f));
    }
}

void Sounds::release(uint32_t handle)
{
    std::lock_guard<std::mutex> held{mutex_};
    if (auto found = holding_.find(handle); found != holding_.end())
    {
        SDL_DestroyAudioStream(found->second.stream);
        holding_.erase(found);
    }
}

void Sounds::refill(const Held& held)
{
    const size_t from = std::min(held.sound->loopSample, held.sound->samples.size());
    SDL_PutAudioStreamData(held.stream, held.sound->samples.data() + from,
                           static_cast<int>((held.sound->samples.size() - from) * sizeof(int16_t)));
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

    // A held sound is topped up before it runs dry rather than restarted when
    // it has: a gap between passes is audible, and half a second in hand is
    // many frames of slack even if one is slow.
    for (auto& [handle, voice] : holding_)
    {
        const int slack = static_cast<int>(voice.sound->sampleRate) * voice.sound->channels *
                          static_cast<int>(sizeof(int16_t)) / 2;
        if (SDL_GetAudioStreamAvailable(voice.stream) < slack)
        {
            refill(voice);
        }
    }
}

size_t Sounds::voices() const
{
    std::lock_guard<std::mutex> held{mutex_};
    return playing_.size() + holding_.size();
}

void Sounds::shutdown()
{
    std::lock_guard<std::mutex> held{mutex_};
    for (SDL_AudioStream* stream : playing_)
    {
        SDL_DestroyAudioStream(stream);
    }
    playing_.clear();
    for (auto& [handle, voice] : holding_)
    {
        SDL_DestroyAudioStream(voice.stream);
    }
    holding_.clear();
    stopped_ = true;
}
} // namespace mh
