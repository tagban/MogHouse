#pragma once

// Screen-space text: the clock, the zone name, the coordinates.
//
// The same atlas and the same fill/outline split the nameplates use, but
// positioned in normalised device coordinates rather than projected from the
// world. One pipeline draws every label, so adding another is a matter of
// pushing a string rather than of writing a shader.
//
// Each label carries its own background alpha, so a panel behind the text
// costs nothing extra - the box is already being tested to know whether the
// fragment is inside the label at all.

namespace mh
{
/// How many labels can be on screen at once.
inline constexpr int kHudStrings = 12;

/// Characters per label. Long enough for a zone name and a timestamp.
inline constexpr int kHudChars = 32;

inline constexpr const char* kHudShader = R"(
struct HudUniforms {
    // Labels in use, one atlas cell in NDC y, the aspect, unused.
    counts : vec4<f32>,
    // Atlas shape: columns, cell size, width, height.
    atlas : vec4<f32>,
    // Per label: left and bottom in NDC, width in cells, background alpha.
    boxes : array<vec4<f32>, 12>,
    // Per label: text colour, then a size multiplier on the shared cell size.
    colours : array<vec4<f32>, 12>,
    // Per glyph: atlas cell index, x offset in cells, advance in cells.
    glyphs : array<vec4<f32>, 384>,
};

const kChars = 32;

@group(0) @binding(0) var<uniform> hud : HudUniforms;
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
    let count = i32(hud.counts.x);
    let baseHeight = hud.counts.y;
    let aspect = hud.counts.z;

    let columns = hud.atlas.x;
    let cell = hud.atlas.y;
    let atlasWidth = hud.atlas.z;
    let atlasHeight = hud.atlas.w;

    var colour = vec3<f32>(0.0);
    var alpha = 0.0;

    for (var slot = 0; slot < count; slot = slot + 1) {
        let box = hud.boxes[slot];
        let widthCells = box.z;
        if (widthCells <= 0.0) {
            continue;
        }

        let scale = hud.colours[slot].w;
        let cellHigh = baseHeight * scale;
        let cellWide = cellHigh / aspect;
        let width = widthCells * cellWide;

        // A little air around the text, which is where the background shows.
        let padX = cellWide * 0.25;
        let padY = cellHigh * 0.12;

        let local = vec2<f32>(in.ndc.x - box.x, in.ndc.y - box.y);
        if (local.x < -padX || local.x >= width + padX ||
            local.y < -padY || local.y >= cellHigh + padY) {
            continue;
        }

        if (box.w > 0.0) {
            colour = vec3<f32>(0.0, 0.0, 0.0);
            alpha = max(alpha, box.w);
        }

        if (local.x < 0.0 || local.x >= width || local.y < 0.0 || local.y >= cellHigh) {
            continue;   // in the padding, not on a glyph
        }

        let xCells = local.x / cellWide;
        for (var i = 0; i < kChars; i = i + 1) {
            let glyph = hud.glyphs[slot * kChars + i];
            let advance = glyph.z;
            if (advance <= 0.0) {
                continue;
            }
            if (xCells < glyph.y || xCells >= glyph.y + advance) {
                continue;
            }

            let insideX = xCells - glyph.y;
            let insideY = 1.0 - local.y / cellHigh;

            let index = i32(glyph.x);
            let col = f32(index % i32(columns));
            let row = f32(index / i32(columns));
            let uv = vec2<f32>((col * cell + insideX * cell) / atlasWidth,
                               (row * cell + insideY * cell) / atlasHeight);
            let sampled = textureSampleLevel(fontTexture, fontSampler, uv, 0.0);

            let fill = sampled.r;
            let outline = sampled.g;
            let together = max(fill, outline);
            if (together > 0.02) {
                colour = mix(vec3<f32>(0.0, 0.0, 0.0), hud.colours[slot].rgb, fill);
                alpha = max(alpha, together);
            }
            break;
        }
    }

    if (alpha <= 0.0) {
        discard;
    }
    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
