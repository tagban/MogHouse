#pragma once

// The sky, from the zone's own colour ramp rather than an invented gradient.
//
// FFXI's sky is not geometry - it is eight colours at eight altitudes, stored
// per time of day alongside the fog and lighting. See docs/lighting-format.md.

namespace pj
{
inline constexpr const char* kSkyShader = R"(
struct SkyUniforms {
    // Camera basis, already scaled so a ray is forward + right*x + up*y across
    // the normalised device square.
    forward : vec4<f32>,
    right : vec4<f32>,
    up : vec4<f32>,
    // Eight stops of the zone's sky ramp. Altitudes are packed one per vec4
    // because a uniform array of f32 strides by 16 bytes anyway.
    colours : array<vec4<f32>, 8>,
    altitudes : array<vec4<f32>, 8>,
    fogColour : vec4<f32>,
};

@group(0) @binding(0) var<uniform> sky : SkyUniforms;

struct SkyOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) direction : vec3<f32>,
};

// One oversized triangle covering the screen: no vertex buffer, no geometry.
@vertex
fn vertexMain(@builtin(vertex_index) index : u32) -> SkyOut {
    var out : SkyOut;
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    let ndc = uv * 2.0 - vec2<f32>(1.0, 1.0);

    out.direction = sky.forward.xyz + sky.right.xyz * ndc.x + sky.up.xyz * ndc.y;
    // z = 1 puts it at the far plane, behind everything drawn after it.
    out.clipPosition = vec4<f32>(ndc, 1.0, 1.0);
    return out;
}

@fragment
fn fragmentMain(in : SkyOut) -> @location(0) vec4<f32> {
    let direction = normalize(in.direction);

    // The ramp runs bottom to top. Find the pair of stops the view direction
    // falls between and blend.
    var colour = sky.colours[0].rgb;
    for (var i = 0u; i < 7u; i = i + 1u) {
        let lo = sky.altitudes[i].x;
        let hi = sky.altitudes[i + 1u].x;
        if (direction.y >= lo && direction.y <= hi) {
            let span = max(hi - lo, 0.0001);
            colour = mix(sky.colours[i].rgb, sky.colours[i + 1u].rgb, (direction.y - lo) / span);
        }
    }
    if (direction.y <= sky.altitudes[0].x) {
        colour = sky.colours[0].rgb;
    }
    if (direction.y >= sky.altitudes[7].x) {
        colour = sky.colours[7].rgb;
    }

    // Below the horizon fades toward the zone's fog colour, so the ground plane
    // meets something of the right hue rather than a hard edge.
    let toGround = smoothstep(0.05, -0.15, direction.y);
    colour = mix(colour, sky.fogColour.rgb, toGround);

    return vec4<f32>(colour, 1.0);
}
)";
} // namespace pj
