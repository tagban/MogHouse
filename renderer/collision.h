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
#include <limits>
#include <optional>
#include <vector>

namespace mh
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
    ///
    /// `maxDrop` bounds how far *below* `near` a surface may be and still
    /// count. Unbounded is right when placing a character with a whole zone to
    /// aim at, and wrong for a footstep: without it, stepping where a stair
    /// tread should be finds the floor underneath the staircase instead and
    /// drops you inside it, and stepping off any edge falls to whatever is
    /// below rather than stopping.
    std::optional<float> groundAt(float x, float z, float near,
                                  float maxDrop = std::numeric_limits<float>::max()) const;

    /// The walkable surface closest to `y`, above or below, however far.
    ///
    /// This is what placing a character wants, and it is a different question
    /// from what a step wants. groundAt answers "what can I step onto from
    /// here", which is bounded by the step height on purpose - ask it to place
    /// a character whose requested height is a couple of units under the floor
    /// they belong on and it will refuse that floor and drop them through the
    /// world instead.
    std::optional<float> closestGroundAt(float x, float z, float y) const;

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

    /// An 8-bit top-down picture of where a character can stand: 255 walkable,
    /// 0 not. Square, covering the same extent the zone map is baked over and
    /// the same way up - north at the top, east to the right - so the two line
    /// up texel for texel.
    ///
    /// Rasterised rather than sampled. Asking groundAt for every texel of a
    /// 1024 square is a million queries; walking the triangles and filling
    /// their footprints touches each one once.
    std::vector<uint8_t> rasteriseWalkable(uint32_t size, const Vec3& centre, float halfExtent) const;

    /// What a step ran into, for working out whether a block is real.
    struct Blocker
    {
        Vec3 normal;
        float lowest{};
        float highest{};
    };

    /// Every wall near a point that straddles a character standing there.
    std::vector<Blocker> blockersNear(const Vec3& at, float radius) const;

    /// How deep the water is over the floor at (x, z) nearest `y`, or nothing
    /// where there is no water.
    ///
    /// FFXI has no swimming. A city's canals and basins are not places you may
    /// walk into and stand at the bottom of, which is exactly what happens with
    /// no notion of water at all: the basin floor is a floor like any other, so
    /// a character walks off the quay, sinks, and stands on the bottom while
    /// the server reports them in deep water.
    std::optional<float> waterDepthAt(float x, float z, float y) const;

    /// How far along `from` -> `to` the first solid face is, as a fraction of
    /// the way, or nothing if the line is clear.
    ///
    /// The camera needs this. Orbiting a character indoors puts the eye through
    /// the wall behind them and leaves you looking at the outside of the house
    /// they are standing in - the game pulls the camera in to the wall instead.
    /// Walls only: floors and ceilings would drag the camera down every time it
    /// looked up a slope.
    std::optional<float> firstWallAlong(const Vec3& from, const Vec3& to) const;

    /// As above, but stopped by floors and ceilings too.
    ///
    /// The camera wants walls only - a floor between the eye and the character
    /// is usually the slope they are standing on. A nameplate wants everything:
    /// a name seen through the ceiling from the floor above is exactly as wrong
    /// as one seen through a wall, and only this tells them apart.
    std::optional<float> firstSolidAlong(const Vec3& from, const Vec3& to) const;

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
        /// The water surface over this face, and whether there is one at all.
        /// MZB puts water on the cell rather than on the geometry, so this is
        /// the instance's height copied onto every triangle it produced.
        bool hasWater;
        float waterY;
    };

    /// Every triangle whose x/z footprint touches this cell.
    const std::vector<uint32_t>& cell(int gx, int gz) const;
    std::optional<float> firstAlong(const Vec3& from, const Vec3& to, bool wallsOnly) const;

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
} // namespace mh
