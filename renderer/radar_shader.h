#pragma once

// The radar: a circular window onto the baked zone map, with whatever is
// nearby drawn on top.
//
// Everything happens in the fragment shader over one triangle. The dots could
// be instanced quads, but a radar shows a few dozen entities at most and a
// bounded loop needs no second pipeline, no vertex buffer and no draw call per
// entity.

namespace mh
{
/// How many entities the radar can show at once. The server only sends what is
/// in range, so this is a ceiling on a list that is usually far shorter.
inline constexpr int kRadarMaxEntities = 96;

inline constexpr const char* kRadarShader = R"(
struct RadarUniforms {
    // Where the radar sits on screen and how big it is, in normalised device
    // coordinates, plus the aspect so the circle stays a circle.
    placement : vec4<f32>,      // centre x, centre y, radius, aspect
    // The square the map was baked over: centre x, centre z, half extent.
    mapExtent : vec4<f32>,
    // Who we are: world x, world z, heading in radians, radar range in units.
    viewer : vec4<f32>,
    // How many of the entity slots are in use.
    counts : vec4<f32>,
    // x, z, kind, unused. Kind: 0 player, 1 npc, 2 enemy.
    entities : array<vec4<f32>, 96>,
};

@group(0) @binding(0) var<uniform> radar : RadarUniforms;
@group(0) @binding(1) var mapTexture : texture_2d<f32>;
@group(0) @binding(2) var maskTexture : texture_2d<f32>;
@group(0) @binding(3) var mapSampler : sampler;

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) ndc : vec2<f32>,
};

@vertex
fn vertexMain(@builtin(vertex_index) index : u32) -> VertexOut {
    // One oversized triangle covering the screen. The radar discards all of it
    // but its own corner, which costs less than it sounds - the rasteriser
    // rejects whole tiles at a time.
    var corners = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -3.0),
        vec2<f32>(-1.0,  1.0),
        vec2<f32>( 3.0,  1.0));

    var out : VertexOut;
    out.ndc = corners[index];
    out.position = vec4<f32>(corners[index], 0.0, 1.0);
    return out;
}

/// Where a point in the world lands in the baked map, as a texture coordinate.
/// The map is north up and east right, so z runs the opposite way to v.
fn mapUv(world : vec2<f32>) -> vec2<f32> {
    let half = radar.mapExtent.z;
    let u = (world.x - (radar.mapExtent.x - half)) / (half * 2.0);
    let v = ((radar.mapExtent.y + half) - world.y) / (half * 2.0);
    return vec2<f32>(u, v);
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let centre = radar.placement.xy;
    let radius = radar.placement.z;
    let aspect = radar.placement.w;

    // Into a space where the radar is a unit circle. x is divided by the
    // aspect so a round radar stays round on a wide window.
    var offset = (in.ndc - centre) / radius;
    offset.x = offset.x * aspect;

    let distance = length(offset);
    if (distance > 1.0) {
        discard;
    }

    // Radar space to world. y on screen is up, which is +z (north).
    let range = radar.viewer.w;
    let world = vec2<f32>(radar.viewer.x + offset.x * range,
                          radar.viewer.y + offset.y * range);

    let uv = mapUv(world);
    var colour = vec3<f32>(0.04, 0.05, 0.07);
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        // textureSampleLevel, not textureSample: the discard above makes
        // control flow non-uniform, and implicit derivatives are illegal
        // there. The map has no mipmaps anyway.
        let terrain = textureSampleLevel(mapTexture, mapSampler, uv, 0.0).rgb;
        let walkable = textureSampleLevel(maskTexture, mapSampler, uv, 0.0).r;

        // Ground you can stand on keeps its colour and is lifted; everything
        // else is pushed down and desaturated, so the shape of where you can
        // go reads at a glance rather than having to be picked out of terrain.
        let flat = vec3<f32>(dot(terrain, vec3<f32>(0.299, 0.587, 0.114)));
        colour = mix(flat * 0.35, terrain * 1.15 + vec3<f32>(0.0, 0.06, 0.0), walkable);
    }

    // Range rings, every quarter of the radar, as a sense of distance.
    let ring = fract(distance * 4.0);
    if (ring < 0.03 && distance > 0.05) {
        colour = mix(colour, vec3<f32>(0.75, 0.80, 0.85), 0.18);
    }

    // Entities. Nearest wins, so a dot on top of another is still one dot
    // rather than a blend of two colours that reads as a third kind.
    let dotRadius = range * 0.045;
    var best = dotRadius;
    var dot = vec3<f32>(0.0);
    var found = false;

    let count = i32(radar.counts.x);
    for (var i = 0; i < count; i = i + 1) {
        let entity = radar.entities[i];
        let away = length(vec2<f32>(entity.x, entity.y) - world);
        if (away < best) {
            best = away;
            found = true;
            let kind = i32(entity.z);
            if (kind == 0) {
                dot = vec3<f32>(0.36, 0.62, 1.00);   // player
            } else if (kind == 2) {
                dot = vec3<f32>(0.95, 0.26, 0.24);   // enemy
            } else {
                dot = vec3<f32>(0.30, 0.85, 0.40);   // npc
            }
        }
    }
    if (found) {
        // A dark rim, so a dot stays legible on ground of any colour.
        let edge = smoothstep(dotRadius, dotRadius * 0.62, best);
        colour = mix(colour * 0.25, dot, edge);
    }

    // Us, at the centre, with a notch showing which way we face.
    let heading = radar.viewer.z;
    let facing = vec2<f32>(sin(heading), cos(heading));
    if (distance < 0.055) {
        colour = vec3<f32>(0.98, 0.98, 1.0);
    } else if (distance < 0.13 && dot2(normalize(offset), facing) > 0.86) {
        colour = vec3<f32>(0.98, 0.98, 1.0);
    }

    // A rim around the whole thing, so it reads as an instrument rather than a
    // hole cut in the view.
    let rim = smoothstep(0.94, 1.0, distance);
    colour = mix(colour, vec3<f32>(0.82, 0.86, 0.92), rim);

    return vec4<f32>(colour, 0.88);
}

fn dot2(a : vec2<f32>, b : vec2<f32>) -> f32 {
    return a.x * b.x + a.y * b.y;
}
)";
} // namespace mh
