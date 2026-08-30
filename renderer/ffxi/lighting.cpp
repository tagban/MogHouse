#include "lighting.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace ffxi
{
namespace
{
/// Colour components are 0..128 rather than 0..255 - alpha is 128 on every
/// entry in the file, which is what gives it away.
constexpr float kComponentScale = 1.0f / 128.0f;

Colour readColour(const std::span<const uint8_t>& data, size_t offset)
{
    uint32_t packed = 0;
    if (offset + sizeof(packed) <= data.size())
    {
        std::memcpy(&packed, data.data() + offset, sizeof(packed));
    }
    return Colour{static_cast<float>(packed & 0xFF) * kComponentScale,
                  static_cast<float>((packed >> 8) & 0xFF) * kComponentScale,
                  static_cast<float>((packed >> 16) & 0xFF) * kComponentScale,
                  static_cast<float>((packed >> 24) & 0xFF) * kComponentScale};
}

float readFloat(const std::span<const uint8_t>& data, size_t offset)
{
    float value = 0.0f;
    if (offset + sizeof(value) <= data.size())
    {
        std::memcpy(&value, data.data() + offset, sizeof(value));
    }
    return value;
}

Colour mix(const Colour& a, const Colour& b, float t)
{
    return Colour{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

float mix(float a, float b, float t)
{
    return a + (b - a) * t;
}
} // namespace

void Lighting::add(const Chunk& chunk)
{
    if (chunk.data.size() < 164)
    {
        return;
    }

    // The id is the time, as four digits: "1200" is noon.
    char id[5] = {};
    std::memcpy(id, chunk.id, 4);
    for (char c : id)
    {
        if (c != 0 && (c < '0' || c > '9'))
        {
            return; // not a time-keyed chunk
        }
    }
    const int clock = std::atoi(id);
    const int minutes = (clock / 100) * 60 + (clock % 100);

    // Several chunks share a time - one per weather type, presumably. The first
    // is taken; picking between them needs the weather, which we do not have.
    if (std::any_of(sets_.begin(), sets_.end(), [&](const LightingSet& s) { return s.minutes == minutes; }))
    {
        return;
    }

    LightingSet set;
    set.minutes = minutes;

    set.sunlight = readColour(chunk.data, 12);
    set.moonlight = readColour(chunk.data, 16);
    set.ambient = readColour(chunk.data, 20);
    set.fog = readColour(chunk.data, 24);
    set.maxFog = readFloat(chunk.data, 28);
    set.minFog = readFloat(chunk.data, 32);
    set.brightness = readFloat(chunk.data, 36);

    set.landscapeSunlight = readColour(chunk.data, 44);
    set.landscapeMoonlight = readColour(chunk.data, 48);
    set.landscapeAmbient = readColour(chunk.data, 52);
    set.landscapeFog = readColour(chunk.data, 56);
    set.landscapeMaxFog = readFloat(chunk.data, 60);
    set.landscapeMinFog = readFloat(chunk.data, 64);
    set.landscapeBrightness = readFloat(chunk.data, 68);

    set.fogColour = readColour(chunk.data, 76);
    set.fogOffset = readFloat(chunk.data, 80);
    set.maxFarClip = readFloat(chunk.data, 88);

    for (size_t i = 0; i < 8; ++i)
    {
        set.skyColours[i] = readColour(chunk.data, 100 + i * 4);
        set.skyAltitudes[i] = readFloat(chunk.data, 132 + i * 4);
    }

    sets_.push_back(set);
    std::sort(sets_.begin(), sets_.end(), [](const LightingSet& a, const LightingSet& b) { return a.minutes < b.minutes; });
}

LightingSet Lighting::at(int minutes) const
{
    if (sets_.empty())
    {
        return LightingSet{};
    }
    if (sets_.size() == 1)
    {
        return sets_.front();
    }

    minutes = ((minutes % 1440) + 1440) % 1440;

    // The last entry at or before this time, and the next one - wrapping round
    // midnight, where the gap spans the end of the day and the start of it.
    const LightingSet* before = &sets_.back();
    const LightingSet* after = &sets_.front();
    for (size_t i = 0; i < sets_.size(); ++i)
    {
        if (sets_[i].minutes <= minutes)
        {
            before = &sets_[i];
            after = &sets_[(i + 1) % sets_.size()];
        }
    }

    int span = after->minutes - before->minutes;
    int into = minutes - before->minutes;
    if (span <= 0)
    {
        span += 1440;
    }
    if (into < 0)
    {
        into += 1440;
    }
    const float t = span ? static_cast<float>(into) / static_cast<float>(span) : 0.0f;

    LightingSet out = *before;
    out.minutes = minutes;
    out.sunlight = mix(before->sunlight, after->sunlight, t);
    out.moonlight = mix(before->moonlight, after->moonlight, t);
    out.ambient = mix(before->ambient, after->ambient, t);
    out.fog = mix(before->fog, after->fog, t);
    out.minFog = mix(before->minFog, after->minFog, t);
    out.maxFog = mix(before->maxFog, after->maxFog, t);
    out.brightness = mix(before->brightness, after->brightness, t);

    out.landscapeSunlight = mix(before->landscapeSunlight, after->landscapeSunlight, t);
    out.landscapeMoonlight = mix(before->landscapeMoonlight, after->landscapeMoonlight, t);
    out.landscapeAmbient = mix(before->landscapeAmbient, after->landscapeAmbient, t);
    out.landscapeFog = mix(before->landscapeFog, after->landscapeFog, t);
    out.landscapeMinFog = mix(before->landscapeMinFog, after->landscapeMinFog, t);
    out.landscapeMaxFog = mix(before->landscapeMaxFog, after->landscapeMaxFog, t);
    out.landscapeBrightness = mix(before->landscapeBrightness, after->landscapeBrightness, t);

    out.fogColour = mix(before->fogColour, after->fogColour, t);
    out.fogOffset = mix(before->fogOffset, after->fogOffset, t);
    out.maxFarClip = mix(before->maxFarClip, after->maxFarClip, t);

    for (size_t i = 0; i < 8; ++i)
    {
        out.skyColours[i] = mix(before->skyColours[i], after->skyColours[i], t);
        out.skyAltitudes[i] = mix(before->skyAltitudes[i], after->skyAltitudes[i], t);
    }
    return out;
}
} // namespace ffxi
