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

        /// Ready to write straight into the instance buffer, column major.
        ///
        /// Built from the transform the car was placed with rather than from
        /// the heading alone. This is a suspended railway - the cars hang from
        /// the beam rather than sitting on top of it - and that hang is a flip
        /// in the placement's own rotation. Composing a fresh Y turn and
        /// calling it the transform threw the flip away and stood all four cars
        /// upright on top of the track, like a train set.
        float transform[16]{};

        /// The rotation it was placed with, and which way the track ran there.
        /// The difference between that heading and the current one is the only
        /// part this is allowed to change.
        float placed[16]{};
        float placedHeading{};
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

                Car car;
                car.instance = instance;
                car.at = translationOf(scene, instance);
                const float* m = &scene.instances[static_cast<size_t>(instance) * 16];
                std::copy(m, m + 16, car.placed);
                std::copy(m, m + 16, car.transform);
                cars_.push_back(car);
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

        for (Car& car : cars_)
        {
            car.placedHeading = headingNear(car.at);
        }

        // Where it calls. The ends are stops in their own right, so a line with
        // none named still behaves the way it did: out, wait, and back.
        stops_ = {0.0f, routeLength_ - train};
        atStop_ = 0;

        travelled_ = 0.0f;
        return true;
    }

    /// Sets where the train calls, as distances along the line from the end it
    /// starts at. The two ends are added if they are not already there, because
    /// a train that runs past the end of its track is worse than one that stops
    /// somewhere odd.
    void setStops(std::vector<float> stops)
    {
        if (!present())
        {
            return;
        }

        const float train = spacing_ * static_cast<float>(cars_.size() - 1);
        const float last = routeLength_ - train;

        stops.push_back(0.0f);
        stops.push_back(last);
        for (float& stop : stops)
        {
            stop = std::clamp(stop, 0.0f, last);
        }

        std::sort(stops.begin(), stops.end());
        stops.erase(std::unique(stops.begin(), stops.end(),
                                [](float a, float b) { return std::fabs(a - b) < 1.0f; }),
                    stops.end());

        stops_ = std::move(stops);

        // Whichever it is nearest, so setting the stops mid-journey does not
        // send it back to the beginning.
        atStop_ = 0;
        for (size_t i = 1; i < stops_.size(); ++i)
        {
            if (std::fabs(stops_[i] - travelled_) < std::fabs(stops_[atStop_] - travelled_))
            {
                atStop_ = i;
            }
        }
    }

    /// Which stop it is standing at, or -1 while it is moving.
    int stop() const { return dwell_ > 0.0f ? static_cast<int>(atStop_) : -1; }

    /// How many stops the line has.
    size_t stops() const { return stops_.size(); }

    /// How far along the line a stop is.
    float stopAt(size_t index) const { return index < stops_.size() ? stops_[index] : 0.0f; }

    /// A point on the line, and which way the track runs there.
    ///
    /// For standing something beside the railway rather than guessing at where
    /// the railway is - the character-select line-up wants the train to pass
    /// behind it, which means knowing where the train will be.
    bool at(float along, Vec3& point, float& heading) const
    {
        if (route_.size() < 2)
        {
            return false;
        }

        float remaining = std::clamp(along, 0.0f, routeLength_);
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
                point = from + (to - from) * t;
                heading = std::atan2(to.x - from.x, to.z - from.z);
                return true;
            }

            remaining -= span;
        }
        return false;
    }

    /// Sets off without the usual wait at the end.
    ///
    /// The sign-in screen opens on a stationary train that then stands there
    /// for nine seconds, which is most of the time anybody looks at it.
    void departNow() { dwell_ = 0.0f; }

    /// Puts the train a few seconds short of somewhere, running towards it.
    ///
    /// Time on this line is measured from when the renderer started, and a
    /// screen the player reaches whenever they finish typing is not. Left to
    /// run, the train is wherever a minute of signing in has left it - halfway
    /// out, or coming back, or standing at an end. This starts its approach
    /// when there is finally somebody to watch it.
    void approach(float target, float seconds)
    {
        if (!present())
        {
            return;
        }

        const float train = spacing_ * static_cast<float>(cars_.size() - 1);

        // travelled_ trails the leading car by the length of the train, so the
        // arithmetic is about where the front of it is rather than the back.
        travelled_ = std::clamp(target - kSpeed * seconds - train, 0.0f, routeLength_ - train);
        forward_ = true;
        dwell_ = 0.0f;
        atStop_ = 0;

        for (size_t i = 0; i < cars_.size(); ++i)
        {
            placeAlong(cars_[i], std::clamp(travelled_ + train - spacing_ * static_cast<float>(i),
                                            0.0f, routeLength_));
        }
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

        // Whether it is moving or waiting, the lamps are always going one way
        // or the other, so this happens before anything returns.
        const float wanted = dwell_ > 0.0f ? 0.0f : 1.0f;
        const float step = seconds / kLampSeconds;
        lamps_ = wanted > lamps_ ? std::min(wanted, lamps_ + step) : std::max(wanted, lamps_ - step);

        if (dwell_ > 0.0f)
        {
            dwell_ -= seconds;

            // The lamps are still fading even though nothing has moved, and the
            // caller has to know to keep drawing them that way.
            return lamps_ > 0.0f;
        }

        const float train = spacing_ * static_cast<float>(cars_.size() - 1);

        travelled_ += kSpeed * seconds * (forward_ ? 1.0f : -1.0f);

        // On to the next stop, turning round at the ends. There is no loop to
        // run - the line has two ends - and a train that teleported back to the
        // start would be the one thing here that could not be believed.
        const size_t next = forward_ ? atStop_ + 1 : (atStop_ == 0 ? 0 : atStop_ - 1);
        const float target = std::clamp(stops_[std::min(next, stops_.size() - 1)], 0.0f, routeLength_ - train);

        if ((forward_ && travelled_ >= target) || (!forward_ && travelled_ <= target))
        {
            travelled_ = target;
            atStop_ = std::min(next, stops_.size() - 1);
            dwell_ = kDwellSeconds;

            // Only the ends turn it round. Everywhere else it carries on the
            // way it was going.
            if (atStop_ + 1 >= stops_.size())
            {
                forward_ = false;
            }
            else if (atStop_ == 0)
            {
                forward_ = true;
            }
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

    /// How brightly the cars are lit, 0 to 1.
    ///
    /// Comes up as it sets off and goes out as it settles, rather than
    /// switching with the motion - lamps warm and dim, and a train whose lights
    /// snapped on the instant it moved would read as a bug in the lights rather
    /// than as a train.
    float lamps() const { return lamps_; }

    /// Where the front of the train is, for anything that wants to sit on it -
    /// a plume of steam, a lamp.
    Vec3 head() const { return cars_.empty() ? Vec3{} : cars_.back().at; }

private:
    /// How fast it runs, in world units a second. Slow: it is scenery at the
    /// far side of a valley, and something crossing a backdrop at a sprint
    /// reads as a mistake.
    static constexpr float kSpeed = 22.0f;

    /// How long it stands at each end before setting off again.
    static constexpr float kDwellSeconds = 9.0f;

    /// How long the lamps take to come up or go out.
    static constexpr float kLampSeconds = 2.5f;

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

    /// Which way the track runs nearest a point, as a heading.
    float headingNear(const Vec3& point) const
    {
        float best = 0.0f;
        float nearest = -1.0f;
        for (size_t i = 0; i + 1 < route_.size(); ++i)
        {
            const Vec3 from = route_[i];
            const Vec3 to = route_[i + 1];
            const Vec3 middle = (from + to) * 0.5f;
            const float d = distance(middle, point);
            if (nearest < 0.0f || d < nearest)
            {
                nearest = d;
                best = std::atan2(to.x - from.x, to.z - from.z);
            }
        }
        return best;
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

                // Along the line, and always the same way along it, whichever
                // way the train is going.
                //
                // Turning the cars round to face the direction of travel is
                // wrong for this train, and wrong only on the return leg, which
                // is why it survived a look: it has a cab at each end - the two
                // mono_a1 are placed facing opposite ways - so it drives from
                // the other end rather than turning round. Rotating them put
                // the rear cab at the front on the way back.
                const Vec3 way = to - from;
                car.heading = std::atan2(way.x, way.z);

                writeTransform(car);
                return;
            }

            remaining -= span;
        }
    }

    /// Turns the car by however much the track has turned since it was parked,
    /// and puts it where it is now.
    ///
    /// The rotation it came with is kept and added to rather than replaced.
    /// That rotation is what hangs it under the beam - this is a suspended
    /// railway, and the cars are upside down relative to anything that rides on
    /// top of a rail. Building a transform out of the heading alone lost it.
    static void writeTransform(Car& car)
    {
        const float turn = car.heading - car.placedHeading;
        const float c = std::cos(turn);
        const float sn = std::sin(turn);

        // A turn about Y applied to each of the placed rotation's columns.
        for (int column = 0; column < 3; ++column)
        {
            const float x = car.placed[column * 4 + 0];
            const float y = car.placed[column * 4 + 1];
            const float z = car.placed[column * 4 + 2];

            car.transform[column * 4 + 0] = c * x + sn * z;
            car.transform[column * 4 + 1] = y;
            car.transform[column * 4 + 2] = -sn * x + c * z;
            car.transform[column * 4 + 3] = 0.0f;
        }

        car.transform[12] = car.at.x;
        car.transform[13] = car.at.y;
        car.transform[14] = car.at.z;
        car.transform[15] = 1.0f;
    }

    std::vector<Vec3> route_;
    std::vector<Car> cars_;
    float routeLength_{};
    float spacing_{21.0f};
    float rideHeight_{};
    float travelled_{};
    std::vector<float> stops_;
    size_t atStop_{0};
    float dwell_{kDwellSeconds};
    float lamps_{0.0f};
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
