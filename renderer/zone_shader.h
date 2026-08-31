#pragma once

// The zone shader, inline rather than loaded from disk so the executable has no
// runtime dependency on its own source tree.
//
// Geometry arrives in model space and is placed by a per-instance matrix, so a
// model appearing 1,218 times is uploaded once rather than 1,218 times.

namespace mh
{
inline constexpr const char* kZoneShader = R"(
struct Uniforms {
    viewProjection : mat4x4<f32>,
    lightDirection : vec4<f32>,
    // From the zone's own lighting for the current time of day.
    ambient : vec4<f32>,
    sunlight : vec4<f32>,
    fogColour : vec4<f32>,
    // x is where fog starts, y where it is total, z the eye position packed
    // alongside so the fragment stage can measure distance.
    fogRange : vec4<f32>,
    eye : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var zoneTexture : texture_2d<f32>;
@group(0) @binding(2) var zoneSampler : sampler;

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
    out.worldPosition = world.xyz;
    out.colour = colour;
    return out;
}

fn shade(in : VertexOut, cutout : bool) -> vec4<f32> {
    let n = normalize(in.normal);
    // Two-sided: FFXI geometry is not consistently wound, and single-sided
    // lighting turns half of a zone into pure shadow.
    let lambert = abs(dot(n, normalize(uniforms.lightDirection.xyz)));

    let sampled = textureSample(zoneTexture, zoneSampler, in.uv);

    // Vertex alpha is stored at quarter scale, so it is multiplied back up
    // before being combined with the texture's. A surface carrying 0.25 here
    // is meant to be fully opaque, and treating it as a quarter is what makes
    // a floor fade away when it is blended.
    let alpha = clamp(4.0 * in.colour.a * sampled.a, 0.0, 1.0);

    // Alpha is a cutout mask only on textures that are black where transparent.
    // On terrain it is a blend factor, and testing it punches holes in the
    // ground - see docs/dxt3-format.md.
    // Tested against the combined alpha rather than the texture's alone, so
    // vertex alpha counts here the same way it does everywhere else.
    if (cutout && alpha < 0.1) {
        discard;
    }

    // Ambient plus a diffuse term, both the zone's own colours for this time of
    // day. Components run 0..128 in the file, so values above 1 are normal and
    // act as overbrightness.
    let light = uniforms.ambient.rgb + uniforms.sunlight.rgb * lambert;
    var colour = sampled.rgb * light;

    // Fog fades toward the zone's fog colour between the two distances it
    // gives. At night the far distance collapses - 123 against noon's 450 - so
    // this is most of what makes FFXI dark after dusk.
    let distance = length(in.worldPosition - uniforms.eye.xyz);
    let fogStart = uniforms.fogRange.x;
    let fogEnd = max(uniforms.fogRange.y, fogStart + 0.001);
    let fog = clamp((distance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    colour = mix(colour, uniforms.fogColour.rgb, fog);

    return vec4<f32>(colour, alpha);
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
} // namespace mh
