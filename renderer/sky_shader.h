#pragma once

// A sky. Not geometry - FFXI's sky is drawn by the engine, which is why seven of
// a zone's textures (cloud, moon, three lens flares) are referenced by no model
// at all. This is a gradient standing in for that until the skybox colour ramp
// in MZB's lighting data is read.

namespace pj
{
inline constexpr const char* kSkyShader = R"(
struct SkyUniforms {
    // Camera basis, each already scaled so a ray is forward + right*x + up*y
    // across the normalised device square. Passing these avoids inverting the
    // view projection just to get a direction back.
    forward : vec4<f32>,
    right : vec4<f32>,
    up : vec4<f32>,
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
    let height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);

    let horizon = vec3<f32>(0.66, 0.72, 0.76);
    let zenith = vec3<f32>(0.24, 0.44, 0.70);
    let ground = vec3<f32>(0.18, 0.18, 0.17);

    // Below the horizon stays dull rather than mirroring the sky, so it reads as
    // distance rather than as a second sky underneath the world.
    var colour = mix(horizon, zenith, smoothstep(0.5, 1.0, height));
    colour = mix(ground, colour, smoothstep(0.44, 0.52, height));
    return vec4<f32>(colour, 1.0);
}
)";
} // namespace pj
