#pragma once

// Water. A translucent sheet at the height each collision cell records, tinted
// by the zone's own lighting so it belongs to the time of day.

namespace mh
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
    // not a water colour. Used as the base they read as wet concrete.
    //
    // The tint is deliberately light. effect umna measures (12,15,29), but that
    // is the sea; taken literally for a river, with the bed hidden behind an
    // opaque sheet, it renders as a black void.
    // Greener and darker than it was, from a side-by-side against a retail
    // client standing in the same spot in Windurst Waters: theirs is a deep
    // green-teal that reads as depth, ours was a pale blue-grey that read as a
    // slab laid on the ground.
    let body = vec3<f32>(0.09, 0.20, 0.17);

    // The ripple sheet, sampled twice drifting at different speeds and angles so
    // it does not read as one sheet sliding.
    let a = textureSample(waterTexture, waterSampler, in.uv + vec2<f32>(time * 0.011, time * 0.007));
    let b = textureSample(waterTexture, waterSampler, in.uv * 0.63 + vec2<f32>(time * -0.008, time * 0.013));
    let foam = clamp(a.a * 0.7 + b.a * 0.55, 0.0, 1.0);

    // Ambient is clamped: components are scaled 0..128, so it can exceed 1, and
    // letting that through once turned the river white.
    let ambient = min(uniforms.ambient.rgb, vec3<f32>(1.0, 1.0, 1.0));
    var colour = body * (0.55 + 0.45 * ambient);
    // The foam was most of why it looked washed out: at 0.40 a bright ripple
    // sheet lifted the whole surface toward grey, which is a lake in overcast
    // daylight rather than a canal.
    colour = colour + ambient * foam * 0.16;

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    // Clear enough to see the bed through it, which is most of what makes water
    // read as water rather than as a coloured lid.
    let alpha = clamp(0.50 + foam * 0.22, 0.0, 0.86);
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
