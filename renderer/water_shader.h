#pragma once

// Water. A translucent sheet at the height each collision cell records, tinted
// by the zone's own lighting so it belongs to the time of day.

namespace pj
{
inline constexpr const char* kWaterShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
    ambient : vec4<f32>,
    sunlight : vec4<f32>,
    fogColour : vec4<f32>,
    fogRange : vec4<f32>,
    eye : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var waterTexture : texture_2d<f32>;
@group(0) @binding(2) var waterSampler : sampler;

struct WaterOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) worldPosition : vec3<f32>,
    @location(1) uv : vec2<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>) -> WaterOut {
    var out : WaterOut;
    out.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
    out.worldPosition = position;
    out.uv = uv;
    return out;
}

@fragment
fn fragmentMain(in : WaterOut) -> @location(0) vec4<f32> {
    let time = uniforms.eye.w;

    // FFXI's own water texture, scrolled. Two samples drifting at different
    // speeds and angles, so the surface does not read as one sheet sliding.
    let a = textureSample(waterTexture, waterSampler, in.uv + vec2<f32>(time * 0.010, time * 0.006));
    let b = textureSample(waterTexture, waterSampler, in.uv * 0.7 + vec2<f32>(time * -0.007, time * 0.011));
    var colour = mix(a.rgb, b.rgb, 0.5);

    // Ambient can exceed 1 because components are scaled 0..128, so it is
    // clamped - an earlier version let it through and the river turned white.
    let ambient = min(uniforms.ambient.rgb, vec3<f32>(1.0, 1.0, 1.0));
    colour = colour * (0.55 + 0.45 * ambient);

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    // The texture's own alpha, softened - it is drawn as a translucent sheet
    // over the bed rather than replacing it.
    let alpha = clamp(mix(a.a, b.a, 0.5) * 1.6, 0.35, 0.85);
    return vec4<f32>(colour, alpha);
}
)";
} // namespace pj
