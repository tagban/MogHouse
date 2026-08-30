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
    let n = normalize(in.normal);
    // Two-sided: FFXI geometry is not consistently wound, and single-sided
    // lighting turns half of a zone into pure shadow.
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let shade = 0.35 + 0.65 * lambert;

    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);
    // Alpha in FFXI textures is a cutout rather than a blend for most surfaces;
    // discarding keeps foliage and railings from being solid rectangles.
    if (sampled.a < 0.35) {
        discard;
    }
    return vec4<f32>(sampled.rgb * shade, 1.0);
}
)";
} // namespace pj
