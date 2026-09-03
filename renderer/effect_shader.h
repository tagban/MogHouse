#pragma once

// The effect shader: the zone shader with a scrolling texture.
//
// Generator-placed meshes - the fountain's jets and flames, a waterfall's
// sheet - are ordinary geometry whose texture the game slides along over
// time, at a rate the generator gives (op 0x28). Everything else here is the
// zone shader's: the same vertex layout and placement matrices, the same
// lighting and fog. The lighting is only half applied, because a flame lit by
// the night's ambient came out black.

namespace mh
{
inline constexpr const char* kEffectShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
    ambient : vec4<f32>,
    sunlight : vec4<f32>,
    fogColour : vec4<f32>,
    fogRange : vec4<f32>,
    eye : vec4<f32>,
};

// x, y: texture scroll in uv per second. z: how much of the zone's lighting
// applies, 0 for self-lit. w: 1 for a sky object - placed relative to the
// eye rather than the world, and never fogged; 0 for scenery.
struct Effect {
    scroll : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var zoneTexture : texture_2d<f32>;
@group(0) @binding(2) var zoneSampler : sampler;
@group(0) @binding(3) var<uniform> effect : Effect;

struct VertexOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) normal : vec3<f32>,
    @location(1) uv : vec2<f32>,
    @location(2) worldPosition : vec3<f32>,
    @location(3) colour : vec4<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>,
              @location(7) colour : vec4<f32>,
              @location(3) m0 : vec4<f32>,
              @location(4) m1 : vec4<f32>,
              @location(5) m2 : vec4<f32>,
              @location(6) m3 : vec4<f32>) -> VertexOut {
    let model = mat4x4<f32>(m0, m1, m2, m3);

    var out : VertexOut;
    let world = model * vec4<f32>(position, 1.0) + vec4<f32>(uniforms.eye.xyz, 0.0) * effect.scroll.w;
    out.clipPosition = uniforms.viewProjection * world;
    out.normal = (model * vec4<f32>(normal, 0.0)).xyz;
    // eye.w carries the animation clock, in seconds.
    out.uv = uv + effect.scroll.xy * uniforms.eye.w;
    out.worldPosition = world.xyz;
    out.colour = colour;
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let n = normalize(in.normal);
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);
    let alpha = clamp(4.0 * in.colour.a * sampled.a, 0.0, 1.0);

    let light = uniforms.ambient.rgb + uniforms.sunlight.rgb * lambert;
    // Vertex colour at half scale, 0x80 meaning full - a sprite frame's
    // 0x808080 leaves the sheet's colour alone, a darker one dims it.
    let tinted = sampled.rgb * min(in.colour.rgb * 2.0, vec3<f32>(1.0, 1.0, 1.0));
    var colour = mix(tinted, tinted * light, effect.scroll.z);

    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0) * (1.0 - effect.scroll.w);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
