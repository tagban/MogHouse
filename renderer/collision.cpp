#include "collision.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mh
{
namespace
{
/// Applies an instance transform and flips Y, matching buildZoneMesh exactly.
/// FFXI's Y axis points down; everything past this point assumes Y is up.
Vec3 toWorld(const float* m, float x, float y, float z)
{
    return Vec3{m[0] * x + m[4] * y + m[8] * z + m[12],
                -(m[1] * x + m[5] * y + m[9] * z + m[13]),
                m[2] * x + m[6] * y + m[10] * z + m[14]};
}

/// Steeper than this is a wall rather than a floor. About 50 degrees, which
/// lets a character climb the ramps and stairs a zone is full of while still
/// stopping at cliffs.
constexpr float kWalkableNormalY = 0.64f;

/// How far above the query point a floor may still be picked up. Walking up a
/// slope puts the next step slightly higher than the current one, and without
/// this the character falls through it.
///
/// It doubles as the step height: anything shorter than this is walked over
/// rather than walked into, and the two have to agree. Blocking on obstacles
/// the ground query would happily carry you onto stops a character dead in
/// open country - Sarutabaruta is covered in ankle-high rocks.
///
/// Swept, not guessed. It was 1.2 - two thirds of a character's height - which
/// climbed the ankle-high rocks it is meant to and also let a character step up
/// onto a bridge railing instead of being stopped by it. The Bastok Markets
/// risers are 0.9, so 0.95 is the tightest value that still climbs a staircase,
/// and zone-wide mobility is unchanged at it: 89% either way.
///
/// This is a stopgap, not the right answer. One global height cannot tell a
/// stair from a railing, because the difference is not height - it is that a
/// railing is too thin to stand on. Testing whether the surface being stepped
/// onto is wide enough to hold a character is the fix; this only narrows the
/// window.
constexpr float kStepUp = 0.95f;

/// How tall the character is, for deciding which walls are in the way. A
/// barrier entirely above their head does not block them.
constexpr float kCharacterHeight = 2.0f;

/// Where a ray straight down from (x, z) crosses this triangle, if it does.
std::optional<float> heightAt(const Vec3& a, const Vec3& b, const Vec3& c, float x, float z)
{
    // Barycentric, in the x/z plane only.
    const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::fabs(d) < 1e-8f)
    {
        return std::nullopt; // edge on, no footprint to stand in
    }

    const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
    const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
    const float w = 1.0f - u - v;

    constexpr float kEdge = -1e-4f; // a hair outside still counts, so seams do not leak
    if (u < kEdge || v < kEdge || w < kEdge)
    {
        return std::nullopt;
    }
    return u * a.y + v * b.y + w * c.y;
}

/// Distance along `direction` at which a segment crosses this triangle, in the
/// x/z plane only - the character is treated as a vertical cylinder, so height
/// is handled by the ground query rather than here.
bool crossesFlat(const Vec3& from, const Vec3& to, const Vec3& a, const Vec3& b)
{
    const float rx = to.x - from.x, rz = to.z - from.z;
    const float sx = b.x - a.x, sz = b.z - a.z;
    const float denominator = rx * sz - rz * sx;
    if (std::fabs(denominator) < 1e-8f)
    {
        return false; // parallel
    }

    const float t = ((a.x - from.x) * sz - (a.z - from.z) * sx) / denominator;
    const float u = ((a.x - from.x) * rz - (a.z - from.z) * rx) / denominator;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}
} // namespace

Collision::Collision(const ffxi::Zone& zone)
{
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};

    for (const ffxi::CollisionInstance& instance : zone.instances)
    {
        if (instance.mesh >= zone.collision.size())
        {
            continue;
        }
        const ffxi::CollisionMesh& mesh = zone.collision[instance.mesh];
        const size_t vertexCount = mesh.vertexCount();

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
            if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            {
                continue;
            }

            Triangle triangle;
            const float* m = instance.transform;
            triangle.a = toWorld(m, mesh.vertices[ia * 3], mesh.vertices[ia * 3 + 1], mesh.vertices[ia * 3 + 2]);
            triangle.b = toWorld(m, mesh.vertices[ib * 3], mesh.vertices[ib * 3 + 1], mesh.vertices[ib * 3 + 2]);
            triangle.c = toWorld(m, mesh.vertices[ic * 3], mesh.vertices[ic * 3 + 1], mesh.vertices[ic * 3 + 2]);
            triangle.normal = normalise(cross(triangle.b - triangle.a, triangle.c - triangle.a));

            // The winding is not consistent across a zone, so a face pointing
            // down is just as much a floor as one pointing up.
            triangle.walkable = std::fabs(triangle.normal.y) >= kWalkableNormalY;

            for (const Vec3& p : {triangle.a, triangle.b, triangle.c})
            {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            triangles_.push_back(triangle);
        }
    }

    if (triangles_.empty())
    {
        return;
    }

    boundsMin_ = lo;
    boundsMax_ = hi;

    // Around a hundred cells across whichever axis is longer. Small enough that
    // a cell holds a handful of triangles, large enough that the grid itself
    // stays a few megabytes.
    const float span = std::max(hi.x - lo.x, hi.z - lo.z);
    cellSize_ = std::max(span / 128.0f, 1.0f);
    cellsX_ = std::max(1, static_cast<int>((hi.x - lo.x) / cellSize_) + 1);
    cellsZ_ = std::max(1, static_cast<int>((hi.z - lo.z) / cellSize_) + 1);
    grid_.resize(static_cast<size_t>(cellsX_) * cellsZ_);

    for (uint32_t index = 0; index < triangles_.size(); ++index)
    {
        const Triangle& triangle = triangles_[index];
        const float minX = std::min({triangle.a.x, triangle.b.x, triangle.c.x});
        const float maxX = std::max({triangle.a.x, triangle.b.x, triangle.c.x});
        const float minZ = std::min({triangle.a.z, triangle.b.z, triangle.c.z});
        const float maxZ = std::max({triangle.a.z, triangle.b.z, triangle.c.z});

        const int x0 = std::clamp(static_cast<int>((minX - lo.x) / cellSize_), 0, cellsX_ - 1);
        const int x1 = std::clamp(static_cast<int>((maxX - lo.x) / cellSize_), 0, cellsX_ - 1);
        const int z0 = std::clamp(static_cast<int>((minZ - lo.z) / cellSize_), 0, cellsZ_ - 1);
        const int z1 = std::clamp(static_cast<int>((maxZ - lo.z) / cellSize_), 0, cellsZ_ - 1);

        for (int gz = z0; gz <= z1; ++gz)
        {
            for (int gx = x0; gx <= x1; ++gx)
            {
                grid_[static_cast<size_t>(gz) * cellsX_ + gx].push_back(index);
            }
        }
    }
}

size_t Collision::wallCount() const
{
    size_t walls = 0;
    for (const Triangle& triangle : triangles_)
    {
        walls += triangle.walkable ? 0 : 1;
    }
    return walls;
}

const std::vector<uint32_t>& Collision::cell(int gx, int gz) const
{
    if (gx < 0 || gz < 0 || gx >= cellsX_ || gz >= cellsZ_)
    {
        return empty_;
    }
    return grid_[static_cast<size_t>(gz) * cellsX_ + gx];
}

void Collision::forEachNear(float minX, float minZ, float maxX, float maxZ, const std::vector<uint32_t>*& single,
                            std::vector<uint32_t>& scratch) const
{
    const int x0 = static_cast<int>(std::floor((minX - boundsMin_.x) / cellSize_));
    const int x1 = static_cast<int>(std::floor((maxX - boundsMin_.x) / cellSize_));
    const int z0 = static_cast<int>(std::floor((minZ - boundsMin_.z) / cellSize_));
    const int z1 = static_cast<int>(std::floor((maxZ - boundsMin_.z) / cellSize_));

    // The common case is one cell, and copying its list would be the most
    // expensive part of the query.
    if (x0 == x1 && z0 == z1)
    {
        single = &cell(x0, z0);
        return;
    }

    scratch.clear();
    for (int gz = z0; gz <= z1; ++gz)
    {
        for (int gx = x0; gx <= x1; ++gx)
        {
            const std::vector<uint32_t>& list = cell(gx, gz);
            scratch.insert(scratch.end(), list.begin(), list.end());
        }
    }
    std::sort(scratch.begin(), scratch.end());
    scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
    single = &scratch;
}

std::optional<float> Collision::groundAt(float x, float z, float near, float maxDrop) const
{
    if (triangles_.empty())
    {
        return std::nullopt;
    }

    const std::vector<uint32_t>* candidates = nullptr;
    std::vector<uint32_t> scratch;
    forEachNear(x, z, x, z, candidates, scratch);

    std::optional<float> best;
    for (uint32_t index : *candidates)
    {
        const Triangle& triangle = triangles_[index];
        if (!triangle.walkable)
        {
            continue;
        }
        const std::optional<float> y = heightAt(triangle.a, triangle.b, triangle.c, x, z);
        if (!y || *y > near + kStepUp || *y < near - maxDrop)
        {
            continue;
        }
        if (!best || *y > *best)
        {
            best = y;
        }
    }
    return best;
}

std::vector<uint8_t> Collision::rasteriseWalkable(uint32_t size, const Vec3& centre, float halfExtent) const
{
    std::vector<uint8_t> mask(static_cast<size_t>(size) * size, 0);
    if (triangles_.empty() || size == 0 || halfExtent <= 0.0f)
    {
        return mask;
    }

    const float scale = static_cast<float>(size) / (halfExtent * 2.0f);
    const float originX = centre.x - halfExtent;

    // Row 0 is the +z edge, so this comes out the same way up as the baked
    // map: north at the top, east to the right. The two are only ever useful
    // together, and a mask that disagreed with the map would put every radar
    // dot the same distance off the terrain it is meant to sit on.
    const float originZ = centre.z + halfExtent;

    for (const Triangle& triangle : triangles_)
    {
        if (!triangle.walkable)
        {
            continue;
        }

        // Into texel space. Only x and z matter - this is a plan view, and a
        // ramp counts as ground wherever its footprint falls.
        const float ax = (triangle.a.x - originX) * scale;
        const float az = (originZ - triangle.a.z) * scale;
        const float bx = (triangle.b.x - originX) * scale;
        const float bz = (originZ - triangle.b.z) * scale;
        const float cx = (triangle.c.x - originX) * scale;
        const float cz = (originZ - triangle.c.z) * scale;

        int minX = static_cast<int>(std::floor(std::min({ax, bx, cx})));
        int maxX = static_cast<int>(std::ceil(std::max({ax, bx, cx})));
        int minZ = static_cast<int>(std::floor(std::min({az, bz, cz})));
        int maxZ = static_cast<int>(std::ceil(std::max({az, bz, cz})));

        minX = std::max(minX, 0);
        minZ = std::max(minZ, 0);
        maxX = std::min(maxX, static_cast<int>(size) - 1);
        maxZ = std::min(maxZ, static_cast<int>(size) - 1);
        if (minX > maxX || minZ > maxZ)
        {
            continue;
        }

        const float area = (bz - az) * (cx - ax) - (bx - ax) * (cz - az);
        if (std::fabs(area) < 1e-9f)
        {
            continue;
        }

        for (int z = minZ; z <= maxZ; ++z)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float px = static_cast<float>(x) + 0.5f;
                const float pz = static_cast<float>(z) + 0.5f;

                // Barycentric, with the winding folded into the sign of the
                // area - collision winding is not consistent across a zone, so
                // testing for "all positive" would drop half the triangles.
                const float w0 = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz));
                const float w1 = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz));
                const float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
                if (std::fabs(d) < 1e-9f)
                {
                    continue;
                }
                const float u = w0 / d;
                const float v = w1 / d;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f)
                {
                    continue;
                }
                mask[static_cast<size_t>(z) * size + x] = 255;
            }
        }
    }
    return mask;
}

std::optional<float> Collision::closestGroundAt(float x, float z, float y) const
{
    if (triangles_.empty())
    {
        return std::nullopt;
    }

    const std::vector<uint32_t>* candidates = nullptr;
    std::vector<uint32_t> scratch;
    forEachNear(x, z, x, z, candidates, scratch);

    std::optional<float> best;
    for (uint32_t index : *candidates)
    {
        const Triangle& triangle = triangles_[index];
        if (!triangle.walkable)
        {
            continue;
        }
        const std::optional<float> height = heightAt(triangle.a, triangle.b, triangle.c, x, z);
        if (!height)
        {
            continue;
        }
        if (!best || std::fabs(*height - y) < std::fabs(*best - y))
        {
            best = height;
        }
    }
    return best;
}

std::optional<Vec3> Collision::nearestGround(float x, float z, float near, float maxRadius) const
{
    if (const std::optional<float> here = closestGroundAt(x, z, near))
    {
        return Vec3{x, *here, z};
    }

    // Rings outward, a cell at a time. Sampling finer than the grid would just
    // re-test the same triangles.
    const int rings = std::max(1, static_cast<int>(maxRadius / cellSize_));
    for (int ring = 1; ring <= rings; ++ring)
    {
        const float radius = ring * cellSize_;
        const int steps = std::max(8, ring * 8);
        for (int i = 0; i < steps; ++i)
        {
            const float angle = 6.28318531f * static_cast<float>(i) / static_cast<float>(steps);
            const float px = x + std::cos(angle) * radius;
            const float pz = z + std::sin(angle) * radius;
            if (const std::optional<float> y = closestGroundAt(px, pz, near))
            {
                return Vec3{px, *y, pz};
            }
        }
    }
    return std::nullopt;
}

std::vector<Collision::Blocker> Collision::blockersNear(const Vec3& at, float radius) const
{
    std::vector<Blocker> found;
    if (triangles_.empty())
    {
        return found;
    }

    const std::vector<uint32_t>* candidates = nullptr;
    std::vector<uint32_t> scratch;
    forEachNear(at.x - radius, at.z - radius, at.x + radius, at.z + radius, candidates, scratch);

    for (uint32_t index : *candidates)
    {
        const Triangle& triangle = triangles_[index];
        if (triangle.walkable)
        {
            continue;
        }
        const float lowest = std::min({triangle.a.y, triangle.b.y, triangle.c.y});
        const float highest = std::max({triangle.a.y, triangle.b.y, triangle.c.y});
        if (highest < at.y + kStepUp || lowest > at.y + kCharacterHeight)
        {
            continue;
        }
        found.push_back(Blocker{triangle.normal, lowest, highest});
    }
    return found;
}

Vec3 Collision::move(const Vec3& from, const Vec3& to, float radius) const
{
    if (triangles_.empty())
    {
        return to;
    }

    const std::vector<uint32_t>* candidates = nullptr;
    std::vector<uint32_t> scratch;
    forEachNear(std::min(from.x, to.x) - radius, std::min(from.z, to.z) - radius, std::max(from.x, to.x) + radius,
                std::max(from.z, to.z) + radius, candidates, scratch);

    // Push the test point out to the character's radius so a shoulder stops at
    // the wall rather than the centre line reaching it first.
    const Vec3 step = to - from;
    const float length = std::sqrt(step.x * step.x + step.z * step.z);
    if (length < 1e-5f)
    {
        return to;
    }
    const Vec3 probe{to.x + step.x / length * radius, to.y, to.z + step.z / length * radius};

    for (uint32_t index : *candidates)
    {
        const Triangle& triangle = triangles_[index];
        if (triangle.walkable)
        {
            continue; // floors do not block
        }

        // Only walls that actually straddle the character matter, and only
        // above the step height - a rock they would simply walk up onto is not
        // an obstacle.
        const float lowest = std::min({triangle.a.y, triangle.b.y, triangle.c.y});
        const float highest = std::max({triangle.a.y, triangle.b.y, triangle.c.y});
        if (highest < from.y + kStepUp || lowest > from.y + kCharacterHeight)
        {
            continue;
        }

        if (crossesFlat(from, probe, triangle.a, triangle.b) || crossesFlat(from, probe, triangle.b, triangle.c) ||
            crossesFlat(from, probe, triangle.c, triangle.a))
        {
            // Slide: keep whatever part of the step runs along the wall.
            const Vec3 n = normalise(Vec3{triangle.normal.x, 0.0f, triangle.normal.z});
            const float into = step.x * n.x + step.z * n.z;
            const Vec3 slid{from.x + step.x - n.x * into, to.y, from.z + step.z - n.z * into};

            // If sliding still crosses something, stop where we were.
            const Vec3 slidProbe{slid.x + (slid.x - from.x), slid.y, slid.z + (slid.z - from.z)};
            for (uint32_t other : *candidates)
            {
                const Triangle& t = triangles_[other];
                if (t.walkable)
                {
                    continue;
                }
                if (crossesFlat(from, slidProbe, t.a, t.b) || crossesFlat(from, slidProbe, t.b, t.c) ||
                    crossesFlat(from, slidProbe, t.c, t.a))
                {
                    return from;
                }
            }
            return slid;
        }
    }
    return to;
}
} // namespace mh
