#pragma once

// Names over heads, drawn from the Verdana atlas in renderer/assets.
//
// Projected in the fragment shader rather than drawn as billboarded quads: the
// radar already loops over every entity per fragment for its dots, and this is
// the same shape of problem with the same bounded list. It costs a loop over a
// few dozen entities per pixel and saves a vertex buffer, a second pipeline and
// a draw call per name.
//
// The loop is cheap because almost every fragment fails a name's bounding box
// on the first test and moves on. Only the handful of fragments actually
// inside a name pay for the glyph search.

namespace mh
{
/// How many names can be shown at once. Past this the bodies still draw, they
/// are just anonymous - which is better than dropping them.
inline constexpr int kNameplateMax = 32;

/// Characters per name. FFXI names are at most 15, and this leaves room for a
/// prefix or a short suffix.
inline constexpr int kNameplateChars = 20;

inline constexpr const char* kNameplateShader = R"(
struct NameplateUniforms {
    // The camera's view-projection, so a world position can be placed on
    // screen without the CPU doing it again.
    viewProjection : mat4x4<f32>,
    // Names in use, text height in NDC, aspect, and the atlas cell size in
    // texels over the atlas width - enough to turn a cell index into UVs.
    counts : vec4<f32>,
    // Atlas shape: columns, cell size, atlas width, atlas height.
    atlas : vec4<f32>,
    // World position per name in xyz; w is the name's total width in cells.
    positions : array<vec4<f32>, 32>,
    // Colour per name. The outline stays black whatever this says.
    colours : array<vec4<f32>, 32>,
    // Per glyph: atlas cell index, x offset in cells, advance in cells.
    // Laid out name-major, kNameplateChars apart.
    glyphs : array<vec4<f32>, 640>,
};

const kChars = 20;

@group(0) @binding(0) var<uniform> plate : NameplateUniforms;
@group(0) @binding(1) var fontTexture : texture_2d<f32>;
@group(0) @binding(2) var fontSampler : sampler;

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

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let count = i32(plate.counts.x);
    let textHeight = plate.counts.y;      // one cell, in NDC y
    let aspect = plate.counts.z;
    let cellWide = textHeight / aspect;   // one cell, in NDC x

    let columns = plate.atlas.x;
    let cell = plate.atlas.y;
    let atlasWidth = plate.atlas.z;
    let atlasHeight = plate.atlas.w;

    var colour = vec3<f32>(0.0);
    var alpha = 0.0;

    for (var slot = 0; slot < count; slot = slot + 1) {
        let clip = plate.viewProjection * vec4<f32>(plate.positions[slot].xyz, 1.0);
        if (clip.w <= 0.0) {
            continue;   // behind the camera
        }
        let screen = clip.xy / clip.w;

        let widthCells = plate.positions[slot].w;
        if (widthCells <= 0.0) {
            continue;
        }

        let width = widthCells * cellWide;
        let left = screen.x - width * 0.5;
        let local = vec2<f32>(in.ndc.x - left, in.ndc.y - screen.y);

        // A cell of slack on the right, for the last glyph reaching past
        // where the pen stopped.
        if (local.x < 0.0 || local.x >= width + cellWide || local.y < 0.0 || local.y >= textHeight) {
            continue;   // the cheap test almost every fragment takes
        }

        // Which glyph of this name the fragment is over. The offsets are
        // already laid out, so this is a search rather than an accumulation.
        // Sampled across the whole cell rather than across the advance -
        // see hud_shader.h, which had the same fault and the same fix. The
        // advance is how far the pen moves; the ink and its outline are wider,
        // and sampling only the advance clips every letter's right edge.
        let xCells = local.x / cellWide;
        var fill = 0.0;
        var outline = 0.0;
        for (var i = 0; i < kChars; i = i + 1) {
            let glyph = plate.glyphs[slot * kChars + i];
            if (glyph.z <= 0.0) {
                continue;
            }
            if (xCells < glyph.y || xCells >= glyph.y + 1.0) {
                continue;
            }

            let insideX = xCells - glyph.y;
            let insideY = 1.0 - local.y / textHeight;

            let index = i32(glyph.x);
            let col = f32(index % i32(columns));
            let row = f32(index / i32(columns));

            let uv = vec2<f32>((col * cell + insideX * cell) / atlasWidth,
                               (row * cell + insideY * cell) / atlasHeight);
            let sampled = textureSampleLevel(fontTexture, fontSampler, uv, 0.0);
            fill = max(fill, sampled.r);
            outline = max(outline, sampled.g);
        }

        // Red is the glyph, green is the outline around it, which is what
        // keeps a light name readable on pale stone with no panel behind it.
        let together = max(fill, outline);
        if (together > 0.02) {
            colour = mix(vec3<f32>(0.0, 0.0, 0.0), plate.colours[slot].rgb, fill);
            alpha = max(alpha, together);
        }
    }

    if (alpha <= 0.0) {
        discard;
    }
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
