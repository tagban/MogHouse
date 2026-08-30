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
    // No water texture is stored anywhere - lotus generates one - so the
    // surface is procedural: crossed waves of different wavelengths and speeds
    // so the pattern does not visibly repeat.
    let time = uniforms.eye.w;
    let p = in.worldPosition.xz;

    let wave1 = sin(p.x * 0.35 + time * 1.1) * cos(p.y * 0.29 - time * 0.8);
    let wave2 = sin((p.x + p.y) * 0.17 - time * 0.6);
    let wave3 = sin(p.y * 0.55 + time * 1.7) * 0.5;
    let height = wave1 * 0.5 + wave2 * 0.35 + wave3 * 0.25;

    // A normal derived from the wave slopes, so the light moves across the
    // surface rather than the colour just pulsing.
    let slopeX = cos(p.x * 0.35 + time * 1.1) * 0.35 * 0.5 + cos((p.x + p.y) * 0.17 - time * 0.6) * 0.17 * 0.35;
    let slopeZ = -sin(p.y * 0.29 - time * 0.8) * 0.29 * 0.5 + cos(p.y * 0.55 + time * 1.7) * 0.55 * 0.125;
    let n = normalize(vec3<f32>(-slopeX, 1.0, -slopeZ));

    let toEye = normalize(uniforms.eye.xyz - in.worldPosition);
    // Water seen edge-on reflects; seen from above it shows its depth.
    let facing = clamp(dot(n, toEye), 0.0, 1.0);
    let fresnel = pow(1.0 - facing, 3.0);

    let deep = vec3<f32>(0.06, 0.19, 0.24);
    let shallow = vec3<f32>(0.16, 0.35, 0.38);
    var colour = mix(deep, shallow, height * 0.5 + 0.5);

    // Sky colour standing in for a reflection at glancing angles.
    let skyish = uniforms.ambient.rgb * 0.9;
    colour = mix(colour, skyish, fresnel * 0.65);

    // A specular glint that travels with the waves.
    let specular = pow(clamp(dot(reflect(-toEye, n), normalize(uniforms.lightDirection.xyz)), 0.0, 1.0), 48.0);
    colour = colour * (uniforms.ambient.rgb + uniforms.sunlight.rgb * 0.5) + uniforms.sunlight.rgb * specular * 0.6;

    // Fogged like everything else, or distant water sits oddly in front of a
    // fogged shore.
    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    // More opaque edge-on, clearer looking straight down, as water is.
    let alpha = mix(0.62, 0.9, fresnel);
    return vec4<f32>(colour, alpha);
}
)";
} // namespace pj
