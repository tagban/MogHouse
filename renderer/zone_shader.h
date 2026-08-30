#pragma once

// The zone shader, inline rather than loaded from disk so the executable has no
// runtime dependency on its own source tree.
//
// Geometry arrives in model space and is placed by a per-instance matrix, so a
// model appearing 1,218 times is uploaded once rather than 1,218 times.

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
              @location(2) uv : vec2<f32>,
              // The placement matrix, one column per attribute.
              @location(3) m0 : vec4<f32>,
              @location(4) m1 : vec4<f32>,
              @location(5) m2 : vec4<f32>,
              @location(6) m3 : vec4<f32>) -> VertexOut {
    let model = mat4x4<f32>(m0, m1, m2, m3);

    var out : VertexOut;
    let world = model * vec4<f32>(position, 1.0);
    out.clipPosition = uniforms.viewProjection * world;
    // Rotating the normal by the same matrix is only exact for uniform scale,
    // which is what placements use.
    out.normal = (model * vec4<f32>(normal, 0.0)).xyz;
    out.uv = uv;
    return out;
}

fn shade(in : VertexOut, cutout : bool) -> vec4<f32> {
    let n = normalize(in.normal);
    // Two-sided: FFXI geometry is not consistently wound, and single-sided
    // lighting turns half of a zone into pure shadow.
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let lit = 0.35 + 0.65 * lambert;

    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);

    // Alpha is a cutout mask only on textures that are black where transparent.
    // On terrain it is a blend factor, and testing it punches holes in the
    // ground - see docs/dxt3-format.md.
    if (cutout && sampled.a < 0.03) {
        discard;
    }
    return vec4<f32>(sampled.rgb * lit, 1.0);
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    return shade(in, false);
}

@fragment
fn fragmentCutout(in : VertexOut) -> @location(0) vec4<f32> {
    return shade(in, true);
}
)";
} // namespace pj
