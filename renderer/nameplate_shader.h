#pragma once

// Names over heads.
//
// Projected in the fragment shader rather than drawn as billboarded quads: the
// radar already loops over every entity per fragment for its dots, and this is
// the same shape of problem with the same bounded list. It costs a loop over a
// few dozen entities per pixel and saves a vertex buffer, a second pipeline and
// a draw call per name.

namespace mh
{
/// How many names can be shown at once. Past this the bodies still draw, they
/// are just anonymous - which is better than dropping them.
inline constexpr int kNameplateMax = 32;

/// Characters per name. FFXI names are at most 15, and this leaves room for a
/// short prefix if one is ever wanted.
inline constexpr int kNameplateChars = 20;

inline constexpr const char* kNameplateShader = R"(
struct NameplateUniforms {
    // The camera's view-projection, so a world position can be placed on
    // screen without the CPU doing it again.
    viewProjection : mat4x4<f32>,
    // How many names are in use, then the glyph size in NDC y, then the aspect.
    counts : vec4<f32>,
    // World position per name, w unused.
    positions : array<vec4<f32>, 32>,
    // Glyphs, kNameplateChars per name, laid out name-major. One character per
    // vec4 because a uniform array strides by 16 bytes regardless.
    glyphs : array<vec4<f32>, 640>,
};

const kGlyphWidth = 4;
const kGlyphHeight = 6;
const kChars = 20;
const kFont = array<u32, 40>(
    0u, 16290430u, 6969727u, 8788062u, 8001663u, 8804735u, 283007u, 16144478u,
    16531775u, 139233u, 8263696u, 8725311u, 8521791u, 16540095u, 16613823u, 8001630u,
    1610367u, 12130398u, 10064511u, 6707554u, 270273u, 8259615u, 4131855u, 16614975u,
    13419315u, 806659u, 9329265u, 8018526u, 8648832u, 10132578u, 6969697u, 16531719u,
    6707559u, 6707550u, 842817u, 6969690u, 8034918u, 192u, 1065220u, 2048u
);

@group(0) @binding(0) var<uniform> plate : NameplateUniforms;

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) ndc : vec2<f32>,
};

@vertex
fn vertexMain(@builtin(vertex_index) index : u32) -> VertexOut {
    var corners = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -3.0),
        vec2<f32>(-1.0,  1.0),
        vec2<f32>( 3.0,  1.0));

    var out : VertexOut;
    out.ndc = corners[index];
    out.position = vec4<f32>(corners[index], 0.0, 1.0);
    return out;
}

/// How many characters of a name are not trailing spaces, so a short name is
/// centred on the body rather than on a padded field.
fn nameLength(slot : i32) -> i32 {
    var length = 0;
    for (var i = 0; i < kChars; i = i + 1) {
        if (i32(plate.glyphs[slot * kChars + i].x) != 0) {
            length = i + 1;
        }
    }
    return length;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let count = i32(plate.counts.x);
    let glyphHeight = plate.counts.y;
    let aspect = plate.counts.z;
    let glyphWidth = glyphHeight / aspect;

    var colour = vec3<f32>(0.0);
    var alpha = 0.0;

    for (var slot = 0; slot < count; slot = slot + 1) {
        let clip = plate.viewProjection * vec4<f32>(plate.positions[slot].xyz, 1.0);
        if (clip.w <= 0.0) {
            continue;   // behind the camera
        }
        let screen = clip.xy / clip.w;

        let length = nameLength(slot);
        if (length == 0) {
            continue;
        }

        let advance = glyphWidth * f32(kGlyphWidth + 1);
        let width = advance * f32(length);
        let left = screen.x - width * 0.5;
        let bottom = screen.y;

        let textHeight = glyphHeight * f32(kGlyphHeight);

        // A panel behind the whole name, with a little air around it, rather
        // than a dark square behind each character. Names sit over stone that
        // is nearly the same value as the glyphs, and per-cell shading leaves
        // them legible only where a stroke happens to fall.
        let padX = glyphWidth * 1.5;
        let padY = glyphHeight * 1.5;
        let local = vec2<f32>(in.ndc.x - left, in.ndc.y - bottom);
        if (local.x < -padX || local.x >= width + padX || local.y < -padY ||
            local.y >= textHeight + padY) {
            continue;
        }

        colour = vec3<f32>(0.02, 0.02, 0.04);
        alpha = max(alpha, 0.55);

        if (local.x < 0.0 || local.x >= width || local.y < 0.0 || local.y >= textHeight) {
            continue;
        }

        let column = i32(local.x / advance);
        let inColumn = i32((local.x - f32(column) * advance) / glyphWidth);
        // Row 0 is the top of the glyph and NDC y runs the other way.
        let row = kGlyphHeight - 1 - i32(local.y / glyphHeight);

        if (column >= length || inColumn >= kGlyphWidth || row < 0 || row >= kGlyphHeight) {
            continue;
        }

        let glyph = kFont[i32(plate.glyphs[slot * kChars + column].x)];
        if (((glyph >> u32(inColumn * kGlyphHeight + row)) & 1u) == 1u) {
            colour = vec3<f32>(0.98, 0.98, 1.0);
            alpha = 1.0;
        }
    }

    if (alpha <= 0.0) {
        discard;
    }
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
