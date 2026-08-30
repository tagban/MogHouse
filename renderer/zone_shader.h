#pragma once

// The zone shader, inline rather than loaded from disk so the executable has no
// runtime dependency on its own source tree.
//
// Flat shading from a face normal, with a fixed key light and a little ambient
// so faces pointing away are still readable. Enough to see the shape of a zone
// and no more - materials and textures come later.

namespace pj
{
inline constexpr const char* kZoneShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;

struct VertexOut {
    @builtin(position) clipPosition : vec4<f32>,
    @location(0) normal : vec3<f32>,
};

@vertex
fn vertexMain(@location(0) position : vec3<f32>, @location(1) normal : vec3<f32>) -> VertexOut {
    var out : VertexOut;
    out.clipPosition = uniforms.viewProjection * vec4<f32>(position, 1.0);
    out.normal = normal;
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let n = normalize(in.normal);
    // Two-sided: collision hulls are not consistently wound, and single-sided
    // lighting makes half of a zone read as pure shadow.
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));
    let shade = 0.18 + 0.82 * lambert;
    let tint = vec3<f32>(0.42, 0.60, 0.48);
    return vec4<f32>(tint * shade, 1.0);
}
)";
} // namespace pj
