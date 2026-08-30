#pragma once

// Two ways of looking at a zone: orbiting it from outside to survey the whole
// thing, and standing in it at roughly player height, which is the only view
// that tells you whether it looks right.

#include "math.h"

#include <algorithm>
#include <cmath>

namespace pj
{
/// FFXI characters are around this tall in world units, judging by the height
/// range of a zone's collision geometry.
inline constexpr float kEyeHeight = 1.7f;

struct Camera
{
    bool orbiting = false;

    // Orbit
    Vec3 target{};
    float distance = 100.0f;

    // Walk
    Vec3 position{};

    // Shared look direction
    float yaw = 0.0f;
    float pitch = 0.0f;

    Vec3 forward() const
    {
        return normalise({std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)});
    }

    Vec3 right() const
    {
        const Vec3 f = forward();
        return normalise(cross(f, Vec3{0.0f, 1.0f, 0.0f}));
    }

    Vec3 eye() const
    {
        if (!orbiting)
        {
            return position;
        }
        const Vec3 f = forward();
        return {target.x - f.x * distance, target.y - f.y * distance, target.z - f.z * distance};
    }

    Vec3 lookAtPoint() const
    {
        if (orbiting)
        {
            return target;
        }
        const Vec3 f = forward();
        return {position.x + f.x, position.y + f.y, position.z + f.z};
    }

    void look(float deltaYaw, float deltaPitch)
    {
        yaw += deltaYaw;
        // Just short of straight up or down, so the view never degenerates.
        pitch = std::clamp(pitch + deltaPitch, -1.55f, 1.55f);
    }

    /// Moves along the view direction. Vertical movement is deliberately
    /// separate from pitch, so looking down does not walk you into the ground.
    void walk(float forwardAmount, float strafeAmount, float verticalAmount)
    {
        const Vec3 f = forward();
        const Vec3 flat = normalise({f.x, 0.0f, f.z});
        const Vec3 r = right();

        position.x += flat.x * forwardAmount + r.x * strafeAmount;
        position.z += flat.z * forwardAmount + r.z * strafeAmount;
        position.y += verticalAmount;
    }
};
} // namespace pj
