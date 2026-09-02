#pragma once

// The monorail that runs through the remnants of Sel Phiner.
//
// The zone is the backdrop the retail client stands its characters in, and it
// ships a complete elevated railway: mono_s1 pylons, mono_r1 and mono_r2 rail
// beams on top of them, and a four-car train - mono_a1 at each end, mono_b1 in
// the middle. Whatever used to drive the train along it was stripped out of the
// DAT, so for years it has sat parked on the northern straight.
//
// This puts it back on the line. Nothing here reads the zone's own data beyond
// where those pieces were placed: the route is the rail beams in order, and the
// train is the cars moved along it.

#include "linalg.h"
#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mh
{
/// How far apart two points are. linalg.h has dot and normalise but no length,
/// and every step of laying a train along a track is a distance.
inline float distance(const Vec3& a, const Vec3& b)
{
    const Vec3 d = a - b;
    return std::sqrt(dot(d, d));
}

class Monorail
{
public:
    /// One car, and where it currently is.
    struct Car
    {
        uint32_t instance{}; ///< index into Scene::instances
        Vec3 at{};
        float heading{}; ///< radians, 0 along +z
    };

    /// Looks for a railway in this scene. False when there is none, which is
    /// every zone but one.
    bool find(const Scene& scene)
    {
        *this = Monorail{};

        route_ = chainRails(scene);
        if (route_.size() < 2)
        {
            return false;
        }
        measure();

        // The cars, in the order they sit along the track, so the train keeps
        // its shape rather than shuffling itself as it goes.
        for (const char* model : {"mono_a1", "mono_b1"})
        {
            auto found = scene.instanceRanges.find(model);
            if (found == scene.instanceRanges.end())
            {
                continue;
            }
            for (uint32_t i = 0; i < found->second.second; ++i)
            {
                const uint32_t instance = found->second.first + i;
                cars_.push_back(Car{instance, translationOf(scene, instance), 0.0f});
            }
        }

        if (cars_.empty())
        {
            route_.clear();
            return false;
        }

        // Spaced along the line in the order they were parked in, measured
        // along whichever axis they actually vary on - they were left in a row,
        // and that row is the train.
        std::sort(cars_.begin(), cars_.end(),
                  [](const Car& a, const Car& b)
                  { return a.at.z != b.at.z ? a.at.z < b.at.z : a.at.x < b.at.x; });

        // How far apart to hold them, from how far apart they were left.
        spacing_ = 21.0f;
        if (cars_.size() > 1)
        {
            const float gap = distance(cars_[1].at, cars_[0].at);
            if (gap > 1.0f && gap < 200.0f)
            {
                spacing_ = gap;
            }
        }

        // The height they ride at. The track is flat - every beam sits within a
        // tenth of a unit of the same height - so this is one number rather
        // than something to interpolate along the route.
        rideHeight_ = cars_.front().at.y;

        // Long enough that the whole train fits on the line with somewhere to
        // wait at each end.
        const float train = spacing_ * static_cast<float>(cars_.size() - 1);
        if (routeLength_ <= train + 2.0f)
        {
            route_.clear();
            cars_.clear();
            return false;
        }

        travelled_ = 0.0f;
        return true;
    }

    /// Whether this zone has a railway at all.
    bool present() const { return !cars_.empty() && route_.size() > 1; }

    /// Whether the train is moving right now rather than waiting at an end.
    bool running() const { return present() && dwell_ <= 0.0f; }

    /// How long the line is, end to end.
    float routeLength() const { return routeLength_; }

    /// Moves the train on, and returns true if anything changed - so a caller
    /// can leave the instance buffer alone while it is standing still.
    bool advance(float seconds)
    {
        if (!present() || seconds <= 0.0f)
        {
            return false;
        }

        if (dwell_ > 0.0f)
        {
            dwell_ -= seconds;
            return false;
        }

        const float train = spacing_ * static_cast<float>(cars_.size() - 1);
        const float run = routeLength_ - train;

        travelled_ += kSpeed * seconds * (forward_ ? 1.0f : -1.0f);

        // At an end it waits, then goes back the way it came. There is no loop
        // to run round - the line has two ends - and a train that teleported
        // back to the start would be the one thing on this screen that could
        // not be believed.
        if (travelled_ >= run)
        {
            travelled_ = run;
            forward_ = false;
            dwell_ = kDwellSeconds;
        }
        else if (travelled_ <= 0.0f)
        {
            travelled_ = 0.0f;
            forward_ = true;
            dwell_ = kDwellSeconds;
        }

        // The leading car is furthest along; the rest trail it by a car's
        // length each, so the train bends around the curves instead of sliding
        // along as one rigid piece.
        for (size_t i = 0; i < cars_.size(); ++i)
        {
            const float along = travelled_ + train - spacing_ * static_cast<float>(i);
            placeAlong(cars_[i], std::clamp(along, 0.0f, routeLength_));
        }

        return true;
    }

    const std::vector<Car>& cars() const { return cars_; }

    /// Where the front of the train is, for anything that wants to sit on it -
    /// a plume of steam, a lamp.
    Vec3 head() const { return cars_.empty() ? Vec3{} : cars_.back().at; }

private:
    /// How fast it runs, in world units a second. Slow: it is scenery at the
    /// far side of a valley, and something crossing a backdrop at a sprint
    /// reads as a mistake.
    static constexpr float kSpeed = 14.0f;

    /// How long it stands at each end before setting off again.
    static constexpr float kDwellSeconds = 9.0f;

    static Vec3 translationOf(const Scene& scene, uint32_t instance)
    {
        const float* m = &scene.instances[static_cast<size_t>(instance) * 16];
        return Vec3{m[12], m[13], m[14]};
    }

    /// The rail beams, put in order along the track.
    ///
    /// Nearest neighbour from the far end. The beams are evenly spaced along a
    /// line that never doubles back or crosses itself, so the nearest one that
    /// has not been used yet is always the next one - which is not true of
    /// track in general, and is true of this track.
    static std::vector<Vec3> chainRails(const Scene& scene)
    {
        std::vector<Vec3> beams;
        for (const char* model : {"mono_r1", "mono_r2"})
        {
            auto found = scene.instanceRanges.find(model);
            if (found == scene.instanceRanges.end())
            {
                continue;
            }
            for (uint32_t i = 0; i < found->second.second; ++i)
            {
                beams.push_back(translationOf(scene, found->second.first + i));
            }
        }

        if (beams.size() < 2)
        {
            return {};
        }

        // Start at an end rather than in the middle: the point furthest from
        // the middle of them all is on one end or the other, and either will do.
        Vec3 middle{};
        for (const Vec3& beam : beams)
        {
            middle = middle + beam;
        }
        middle = middle * (1.0f / static_cast<float>(beams.size()));

        size_t start = 0;
        float furthest = -1.0f;
        for (size_t i = 0; i < beams.size(); ++i)
        {
            const float d = distance(beams[i], middle);
            if (d > furthest)
            {
                furthest = d;
                start = i;
            }
        }

        std::vector<bool> used(beams.size(), false);
        std::vector<Vec3> ordered;
        ordered.push_back(beams[start]);
        used[start] = true;

        for (size_t step = 1; step < beams.size(); ++step)
        {
            size_t next = beams.size();
            float best = 0.0f;
            for (size_t i = 0; i < beams.size(); ++i)
            {
                if (used[i])
                {
                    continue;
                }
                const float d = distance(beams[i], ordered.back());
                if (next == beams.size() || d < best)
                {
                    next = i;
                    best = d;
                }
            }
            if (next == beams.size())
            {
                break;
            }
            used[next] = true;
            ordered.push_back(beams[next]);
        }

        return ordered;
    }

    /// Puts a car at a distance along the route, facing the way it is going.
    void placeAlong(Car& car, float along) const
    {
        float remaining = along;
        for (size_t i = 0; i + 1 < route_.size(); ++i)
        {
            const Vec3 from = route_[i];
            const Vec3 to = route_[i + 1];
            const float span = distance(to, from);
            if (span <= 0.0001f)
            {
                continue;
            }

            if (remaining <= span || i + 2 == route_.size())
            {
                const float t = std::clamp(remaining / span, 0.0f, 1.0f);
                const Vec3 at = from + (to - from) * t;
                car.at = Vec3{at.x, rideHeight_, at.z};

                // Along the line it is on, turned round when it is going the
                // other way - a train reverses rather than driving backwards.
                const Vec3 way = forward_ ? (to - from) : (from - to);
                car.heading = std::atan2(way.x, way.z);
                return;
            }

            remaining -= span;
        }
    }

    std::vector<Vec3> route_;
    std::vector<Car> cars_;
    float routeLength_{};
    float spacing_{21.0f};
    float rideHeight_{};
    float travelled_{};
    float dwell_{kDwellSeconds};
    bool forward_{true};


    /// Sums the route once it is built.
    void measure()
    {
        routeLength_ = 0.0f;
        for (size_t i = 0; i + 1 < route_.size(); ++i)
        {
            routeLength_ += distance(route_[i + 1], route_[i]);
        }
    }
};
} // namespace mh
