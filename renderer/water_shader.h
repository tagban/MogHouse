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

struct WaterOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) worldPosition : vec3<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>) -> WaterOut {
    var out : WaterOut;
    out.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
    out.worldPosition = position;
    return out;
}

@fragment
fn fragmentMain(in : WaterOut) -> @location(0) vec4<f32> {
    // No water texture is stored - lotus generates one procedurally - so this
    // is a flat tint until something better exists. Lit by the zone's ambient so
    // it darkens at night with everything else.
    let base = vec3<f32>(0.13, 0.28, 0.34);
    var colour = base * (uniforms.ambient.rgb + uniforms.sunlight.rgb * 0.5);

    // Fogged like everything else, or distant water sits oddly in front of a
    // fogged shore.
    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    return vec4<f32>(colour, 0.72);
}
)";
} // namespace pj
