#pragma once

// Asking the zone where the ground is and what you can walk into.
//
// The collision meshes have been in hand since the zone reader landed - they
// are what the untextured collision view draws. Nothing ever queried them.
// This turns them into two questions:
//
//     groundAt(x, z, near)   how high is the floor here
//     move(from, to, radius) where does this step actually end
//
// The triangles are baked into world space once and bucketed on x and z. A
// zone is around 30,000 collision triangles spread over 1,500 units, so a
// linear scan per step would be thousands of tests per frame and a grid makes
// it tens.

#include "ffxi/mzb.h"
#include "linalg.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace pj
{
/// The zone's collision geometry, indexed for queries.
class Collision
{
public:
    Collision() = default;
    explicit Collision(const ffxi::Zone& zone);

    bool empty() const { return triangles_.empty(); }
    size_t triangleCount() const { return triangles_.size(); }
    size_t wallCount() const;

    /// The walkable surface at (x, z), or nothing if there is none.
    ///
    /// `near` is where the query is coming from: the highest surface at or
    /// below it wins, so standing on a bridge does not drop you to the riverbed
    /// underneath. A small tolerance above `near` is allowed so that walking up
    /// a slope does not fall through the step in front of you.
    std::optional<float> groundAt(float x, float z, float near) const;

    /// The nearest point with ground under it, searching outward from (x, z).
    ///
    /// A zone's bounding box is a rectangle and the zone is not, so an obvious
    /// starting point like the centre often has nothing beneath it - about a
    /// third of East Sarutabaruta's box is outside the map. Dropping a
    /// character there and leaving it in the air is worse than moving it a few
    /// units to somewhere it can stand.
    std::optional<Vec3> nearestGround(float x, float z, float near, float maxRadius) const;

    /// Slides a step against the walls and returns where it actually ends.
    ///
    /// A destination test is not enough. FindNearestPoly-style "is the end
    /// point walkable" checks pass straight through a wall, because the floor
    /// on the far side is walkable - which is exactly how a character ends up
    /// standing inside a cliff. This tests the path.
    Vec3 move(const Vec3& from, const Vec3& to, float radius) const;

    /// What a step ran into, for working out whether a block is real.
    struct Blocker
    {
        Vec3 normal;
        float lowest{};
        float highest{};
    };

    /// Every wall near a point that straddles a character standing there.
    std::vector<Blocker> blockersNear(const Vec3& at, float radius) const;

    Vec3 boundsMin() const { return boundsMin_; }
    Vec3 boundsMax() const { return boundsMax_; }

private:
    struct Triangle
    {
        Vec3 a, b, c;
        Vec3 normal;
        /// True when the face is shallow enough to stand on. Anything steeper
        /// is a wall, and the two are queried differently.
        bool walkable;
    };

    /// Every triangle whose x/z footprint touches this cell.
    const std::vector<uint32_t>& cell(int gx, int gz) const;
    void forEachNear(float minX, float minZ, float maxX, float maxZ,
                     const std::vector<uint32_t>*& single, std::vector<uint32_t>& scratch) const;

    std::vector<Triangle> triangles_;
    std::vector<std::vector<uint32_t>> grid_;
    std::vector<uint32_t> empty_;
    Vec3 boundsMin_{};
    Vec3 boundsMax_{};
    float cellSize_{8.0f};
    int cellsX_{1};
    int cellsZ_{1};
};
} // namespace pj
