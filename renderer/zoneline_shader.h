#pragma once

// The glowing band standing in a zone line.
//
// Only the client can ask to change zone, so it always knew where these were;
// the player did not. Walking to the edge of a zone and feeling for the
// boundary only works if you already know the zone, which is fine for someone
// who played for years and useless for everybody else.
//
// What the server keeps is a point and a box, not a wall - so what is drawn is
// a ring around the point rather than a line across a doorway, which is an
// honest picture of what is actually known. It stands about waist high on a
// hume so it reads as something to walk into rather than something underfoot,
// and it is brightest at the bottom and fades out towards the top so it does
// not become a fence across the view.
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

/// How far up the band reaches, in world units. A hume is about 1.8 tall.
inline constexpr float kZoneLineHeight = 0.95f;

inline constexpr const char* kZoneLineShader = R"(
struct ZoneLineUniforms {
    viewProjection : mat4x4<f32>,
    // Rings in use, band height, seconds for the pulse, and which ring is the
    // target - negative when nothing is selected.
    counts : vec4<f32>,
    // Per line: x, y, z, radius.
    lines : array<vec4<f32>, 16>,
};

@group(0) @binding(0) var<uniform> markers : ZoneLineUniforms;

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    // How far up the band this fragment is, 0 at the ground.
    @location(0) height : f32,
    // Around the ring, for the travelling highlight.
    @location(1) around : f32,
    // Non-zero when this ring marks the current target rather than a zone line.
    @location(2) target : f32,
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
    let angle = (f32(segment) + f32(side)) / segments * 6.28318530718;
    let height = markers.counts.y * f32(top);

    let world = vec3<f32>(line.x + cos(angle) * line.w,
                          line.y + height,
                          line.z + sin(angle) * line.w);

    var out : VertexOut;
    out.position = markers.viewProjection * vec4<f32>(world, 1.0);
    out.height = f32(top);
    out.around = (f32(segment) + f32(side)) / segments;
    out.target = select(0.0, 1.0, f32(instance) == markers.counts.w);
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    // Solid at the foot, gone by the top, so it reads as a glow rising off
    // the ground rather than a wall standing on it.
    let rise = 1.0 - in.height;
    var glow = rise * rise * 0.55;

    // A slow highlight travelling round the ring. Movement is what makes it
    // read as active rather than as a texture someone left on the floor.
    let sweep = fract(in.around - markers.counts.z * 0.15);
    glow = glow + smoothstep(0.85, 1.0, 1.0 - abs(sweep - 0.5) * 2.0) * rise * 0.35;

    // Cyan-white for a zone line: bright against grass, stone and water alike,
    // and not a colour any nameplate uses. A target ring is amber instead, so
    // the two never read as the same thing - one is somewhere to walk, the
    // other is someone to talk to.
    let line = mix(vec3<f32>(0.35, 0.85, 1.0), vec3<f32>(0.85, 0.98, 1.0), rise);
    let mark = mix(vec3<f32>(1.00, 0.72, 0.25), vec3<f32>(1.00, 0.93, 0.70), rise);
    let tint = mix(line, mark, in.target);
    return vec4<f32>(tint * glow, glow);
}
)";
} // namespace mh
