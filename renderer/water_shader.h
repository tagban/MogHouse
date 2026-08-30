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

    // Two layers, which is how FFXI builds it. effect kaw1 and ike1 are pure
    // white - average RGB (255,255,255) - so they are a foam and ripple sheet,
    // not a water colour. Used as the base they read as wet concrete. The body
    // colour lives in effect umna at (12,15,29).
    let body = vec3<f32>(0.055, 0.115, 0.145);

    // The ripple sheet, sampled twice drifting at different speeds and angles so
    // it does not read as one sheet sliding.
    let a = textureSample(waterTexture, waterSampler, in.uv + vec2<f32>(time * 0.011, time * 0.007));
    let b = textureSample(waterTexture, waterSampler, in.uv * 0.63 + vec2<f32>(time * -0.008, time * 0.013));
    // Its alpha is the mask; where both layers agree the highlight is brightest.
    let foam = clamp(a.a * 0.65 + b.a * 0.5, 0.0, 1.0);

    // Ambient is clamped: components are scaled 0..128, so it can exceed 1 and
    // an earlier version let that through and turned the river white.
    let ambient = min(uniforms.ambient.rgb, vec3<f32>(1.0, 1.0, 1.0));
    var colour = body * (0.5 + 0.5 * ambient);
    colour = colour + ambient * foam * 0.22;

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    return vec4<f32>(colour, 0.78);
}
)";
} // namespace pj
