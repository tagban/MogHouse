#pragma once

// The glowing band standing in a zone line.
//
// Only the client can ask to change zone, so it always knew where these were;
// the player did not. Walking to the edge of a zone and feeling for the
// boundary only works if you already know the zone, which is fine for someone
// who played for years and useless for everybody else.
//
// What the server keeps is a point and nothing else. LandSandBoat's zone line
// record is `from`, `to`, `at` and `scale`, and `scale` is bound to
// destinationScaleX/Z - the box you arrive into at the far end, not the one you
// walk through at this one. So a ring was drawn around the point, sized from
// the wrong zone's box, because that was what was in the data.
//
// A zone line is a doorway, though, and a doorway is somewhere the walkable
// ground is narrow. So the direction is measured instead: the narrowest
// walkable span through the point is the opening it stands in, and the band is
// laid across that. Measured from the collision the player actually walks on
// rather than taken from a field that means something else.
//
// It is brightest at the bottom and fades out towards the top so it does not
// become a fence across the view.
//
// One instance per line, one ring of segments each, no vertex buffer: the
// positions are computed from the vertex index against a uniform array, the
// same way the nameplates are.

namespace mh
{
/// How many zone lines can be drawn at once. Zones have a handful of exits.
inline constexpr int kZoneLineMarkers = 16;

/// Segments around each ring. Enough that the curve reads as one at a walk.
inline constexpr int kZoneLineSegments = 48;

/// How far up the band reaches, in world units. A hume is about 1.8 tall and a
/// Tarutaru barely over 1, so waist high on the tallest race stood over the
/// head of the shortest - this sits around the chest of a Tarutaru and the
/// thigh of a Galka, which reads as a marker to either.
inline constexpr float kZoneLineHeight = 0.60f;

inline constexpr const char* kZoneLineShader = R"(
struct ZoneLineUniforms {
    viewProjection : mat4x4<f32>,
    // Rings in use, band height, seconds for the pulse, and which ring is the
    // target - negative when nothing is selected.
    counts : vec4<f32>,
    // Per line: x, y, z, radius.
    lines : array<vec4<f32>, 16>,
    // Per line: direction across the opening (x, z), half its length, and
    // whether this one is a band at all - a target marker stays a ring.
    axes : array<vec4<f32>, 16>,
};

@group(0) @binding(0) var<uniform> markers : ZoneLineUniforms;

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    // How far up the band this fragment is, 0 at the ground.
    @location(0) height : f32,
    // Around the ring, for the travelling highlight.
    @location(1) around : f32,
    // Non-zero when this ring marks the current target rather than a zone line.
    @location(2) selected : f32,   // 'target' is reserved in WGSL
};

@vertex
fn vertexMain(@builtin(vertex_index) vertex : u32,
              @builtin(instance_index) instance : u32) -> VertexOut {
    let line = markers.lines[instance];

    // Two triangles per segment, laid out as a strip would be but written
    // out so no index buffer is needed.
    let segment = vertex / 6u;
    let corner = vertex % 6u;

    // Which end of the segment, and whether this corner is at the top.
    var side = 0u;
    var top = 0u;
    switch corner {
        case 0u: { side = 0u; top = 0u; }
        case 1u: { side = 1u; top = 0u; }
        case 2u: { side = 0u; top = 1u; }
        case 3u: { side = 1u; top = 0u; }
        case 4u: { side = 1u; top = 1u; }
        default: { side = 0u; top = 1u; }
    }

    let segments = 48.0;
    let along = (f32(segment) + f32(side)) / segments;
    let height = markers.counts.y * f32(top);

    let axis = markers.axes[instance];

    // A ring for a target marker, a straight band for a zone line. The ring
    // stands around one person and has no direction to it; the zone line lies
    // across an opening and does.
    let angle = along * 6.28318530718;
    let ring = vec3<f32>(line.x + cos(angle) * line.w,
                         line.y + height,
                         line.z + sin(angle) * line.w);

    let reach = (along - 0.5) * 2.0 * axis.z;
    let band = vec3<f32>(line.x + axis.x * reach,
                         line.y + height,
                         line.z + axis.y * reach);

    let world = select(ring, band, axis.w > 0.5);

    var out : VertexOut;
    out.position = markers.viewProjection * vec4<f32>(world, 1.0);
    out.height = f32(top);
    out.around = along;
    out.selected = select(0.0, 1.0, f32(instance) == markers.counts.w);
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    // Solid at the foot, gone by the top, so it reads as a glow rising off
    // the ground rather than a wall standing on it.
    let rise = 1.0 - in.height;
    var glow = rise * rise * 0.55;

    // A slow highlight travelling along it. Movement is what makes it read as
    // active rather than as a texture someone left on the floor.
    let sweep = fract(in.around - markers.counts.z * 0.15);
    glow = glow + smoothstep(0.85, 1.0, 1.0 - abs(sweep - 0.5) * 2.0) * rise * 0.35;

    // Cyan-white for a zone line: bright against grass, stone and water alike,
    // and not a colour any nameplate uses. A target ring is amber instead, so
    // the two never read as the same thing - one is somewhere to walk, the
    // other is someone to talk to.
    let line = mix(vec3<f32>(0.35, 0.85, 1.0), vec3<f32>(0.85, 0.98, 1.0), rise);
    let mark = mix(vec3<f32>(1.00, 0.72, 0.25), vec3<f32>(1.00, 0.93, 0.70), rise);
    let tint = mix(line, mark, in.selected);
    return vec4<f32>(tint * glow, glow);
}
)";
} // namespace mh
