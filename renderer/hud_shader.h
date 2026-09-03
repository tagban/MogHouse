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
inline constexpr int kHudStrings = 24;

/// Characters per label. Long enough for a zone name and a timestamp.
inline constexpr int kHudChars = 48;

/// Filled rectangles drawn under the labels: the HP, MP and TP bars, and
/// whatever else wants a meter rather than a number.
inline constexpr int kHudBars = 8;

inline constexpr const char* kHudShader = R"(
struct HudUniforms {
    // Labels in use, one atlas cell in NDC y, the aspect, unused.
    counts : vec4<f32>,
    // Atlas shape: columns, cell size, width, height.
    atlas : vec4<f32>,
    // Per label: left and bottom in NDC, width in cells, background alpha.
    boxes : array<vec4<f32>, 24>,
    // Per label: text colour, then a size multiplier on the shared cell size.
    colours : array<vec4<f32>, 24>,
    // Per glyph: atlas cell index, x offset in cells, advance in cells.
    glyphs : array<vec4<f32>, 1152>,
    // Per bar: left, bottom, width, height in NDC. Width zero is no bar.
    bars : array<vec4<f32>, 8>,
    // Per bar: colour, then how opaque it is.
    barColours : array<vec4<f32>, 8>,
};

const kChars = 48;

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

    // Bars first, so a label placed over one draws on top of it: the text
    // reads off the bar rather than the bar painting over the text.
    for (var bar = 0; bar < 8; bar = bar + 1) {
        let rect = hud.bars[bar];
        if (rect.z <= 0.0) {
            continue;
        }
        if (in.ndc.x < rect.x || in.ndc.x >= rect.x + rect.z ||
            in.ndc.y < rect.y || in.ndc.y >= rect.y + rect.w) {
            continue;
        }
        let paint = hud.barColours[bar];
        // Later bars sit on earlier ones - the fill over its track - so this
        // blends rather than replaces.
        colour = mix(colour, paint.rgb, paint.a);
        alpha = max(alpha, paint.a);
    }

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
        // Wide enough on the right for the last glyph's full cell, which
        // reaches past where the pen stopped.
        let padX = cellWide * 0.9;
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

        // A cell of slack on the right, because the last glyph reaches past
        // where the pen stopped. Without it the box clips the final letter
        // even though the sampling no longer does.
        if (local.x < 0.0 || local.x >= width + cellWide || local.y < 0.0 || local.y >= cellHigh) {
            continue;   // in the padding, not on a glyph
        }

        // Each glyph is sampled across its whole cell, not across its
        // advance.
        //
        // The advance is how far the pen moves on to the next letter; the ink,
        // and especially the outline around it, is wider than that. Sampling
        // only the advance cut the right hand edge off every letter - most
        // visibly on narrow ones, where the outline is a larger share of the
        // width.
        //
        // Cells therefore overlap, so this cannot stop at the first hit: it
        // takes the strongest coverage of any glyph over this fragment, which
        // is also what makes neighbouring outlines join up instead of
        // stopping dead against each other.
        let xCells = local.x / cellWide;
        var fill = 0.0;
        var outline = 0.0;
        for (var i = 0; i < kChars; i = i + 1) {
            let glyph = hud.glyphs[slot * kChars + i];
            if (glyph.z <= 0.0) {
                continue;
            }
            if (xCells < glyph.y || xCells >= glyph.y + 1.0) {
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
            fill = max(fill, sampled.r);
            outline = max(outline, sampled.g);
        }

        let together = max(fill, outline);
        if (together > 0.02) {
            // Over a bar the outline has to blend in rather than replace, or
            // every letter carries a hard black halo across the meter.
            let ink = mix(vec3<f32>(0.0, 0.0, 0.0), hud.colours[slot].rgb, fill);
            colour = mix(colour, ink, together);
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
