#pragma once

// The zone shader, inline rather than loaded from disk so the executable has no
// runtime dependency on its own source tree.

namespace pj
{
inline constexpr const char* kZoneShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var zoneTexture : texture_2d<f32>;
@group(0) @binding(2) var zoneSampler : sampler;

struct VertexOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) normal : vec3<f32>,
    @location(1) uv : vec2<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>,
              @location(1) normal : vec3<f32>,
              @location(2) uv : vec2<f32>) -> VertexOut {
    var out : VertexOut;
    out.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
    out.normal = normal;
    out.uv = uv;
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    return shade(in, false);
}

// Same shading, but alpha is treated as a cutout. Used only for the surfaces
// whose mesh header asks for it.
@fragment
fn fragmentCutout(in : VertexOut) -> @location(0) vec4<f32> {
    return shade(in, true);
}

fn shade(in : VertexOut, cutout : bool) -> vec4<f32> {
    let n = normalize(in.normal);
    // Two-sided: FFXI geometry is not consistently wound, and single-sided
    // lighting turns half of a zone into pure shadow.
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let lit = 0.35 + 0.65 * lambert;

    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);

    // Alpha is only a cutout on surfaces that ask for it. On terrain it is a
    // blend factor - sar_kk2, the flat ground, is 60.5% alpha-zero while its
    // RGB is never black - so testing against it punched the ground full of
    // holes and left a checkerboard with the background showing through.
    if (cutout && sampled.a < 0.03) {
        discard;
    }
    return vec4<f32>(sampled.rgb * lit, 1.0);
}
)";
} // namespace pj
