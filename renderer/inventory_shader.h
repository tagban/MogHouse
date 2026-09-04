#pragma once

// The bags: item icons, their counts, and the panel behind them.
//
// A new pipeline rather than more of the HUD, because the HUD is a typeface.
// Its atlas holds letters and it draws them by testing every label against
// every fragment of the screen, which is right for a dozen short strings and
// wrong for eighty pictures. This draws instanced quads instead - one per
// icon, per frame, per digit - so the cost is the area covered rather than the
// screen.
//
// One pass, three kinds of quad, told apart by a mode:
//
//   0  a flat colour, which is the panel and the slot frames
//   1  a cell of the item atlas, which is an icon
//   2  a cell of the font atlas, which is a letter
//
// Two textures bound at once so a count can sit on its icon without a second
// pass and without the two disagreeing about what is on top.

namespace mh
{
/// Quads in one panel: eighty slots and their frames, the counts on them, the
/// bag tabs, and the name of whatever is under the pointer.
inline constexpr int kInventoryQuads = 512;

/// Cells across the item atlas, which is square. 16 x 16 of 32 pixels is a
/// 512-pixel texture holding 256 distinct items - more than a full set of bags
/// can hold in practice, since a stack of ninety-nine arrows is one icon.
inline constexpr int kIconAtlasCells = 16;

/// One icon, in pixels. Every item DAT ships this size.
inline constexpr int kIconSize = 32;

inline constexpr const char* kInventoryShader = R"(
struct InventoryUniforms {
    // Quads in use, aspect, and the item atlas in cells across and down.
    counts : vec4<f32>,
    // Font atlas shape: columns, cell size, width, height.
    font : vec4<f32>,
    // Per quad: left and bottom in NDC, then width and height.
    rects : array<vec4<f32>, 512>,
    // Per quad: mode, cell index, unused, unused.
    looks : array<vec4<f32>, 512>,
    // Per quad: colour, then how opaque it is.
    tints : array<vec4<f32>, 512>,
};

@group(0) @binding(0) var<uniform> inv : InventoryUniforms;
@group(0) @binding(1) var fontTexture : texture_2d<f32>;
@group(0) @binding(2) var fontSampler : sampler;
@group(0) @binding(3) var iconTexture : texture_2d<f32>;
@group(0) @binding(4) var iconSampler : sampler;

struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(0) local : vec2<f32>,
    @location(1) @interpolate(flat) quad : u32,
};

@vertex
fn vertexMain(@builtin(vertex_index) vertex : u32,
              @builtin(instance_index) instance : u32) -> VertexOut {
    // Two triangles, corners derived rather than fetched: a quad this simple
    // does not earn a vertex buffer.
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0),
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 1.0));

    let corner = corners[vertex];
    let rect = inv.rects[instance];

    var out : VertexOut;
    out.local = corner;
    out.quad = instance;
    out.position = vec4<f32>(rect.x + corner.x * rect.z, rect.y + corner.y * rect.w, 0.0, 1.0);
    return out;
}

@fragment
fn fragmentMain(in : VertexOut) -> @location(0) vec4<f32> {
    let look = inv.looks[in.quad];
    let tint = inv.tints[in.quad];
    let mode = i32(look.x);

    if (mode == 0) {
        return vec4<f32>(tint.rgb * tint.a, tint.a);
    }

    if (mode == 1) {
        let across = inv.counts.z;
        let down = inv.counts.w;
        let cell = i32(look.y);
        let col = f32(cell % i32(across));
        let row = f32(cell / i32(across));

        // The icon was written into the atlas top row first, and NDC y counts
        // upward, so the sample walks the cell the other way.
        let uv = vec2<f32>((col + in.local.x) / across, (row + (1.0 - in.local.y)) / down);
        let sampled = textureSampleLevel(iconTexture, iconSampler, uv, 0.0);
        return vec4<f32>(sampled.rgb * sampled.a * tint.a, sampled.a * tint.a);
    }

    // A letter. Fill in red, outline in green, the same as every other pass
    // that draws this atlas.
    let columns = inv.font.x;
    let cell = inv.font.y;
    let atlasWidth = inv.font.z;
    let atlasHeight = inv.font.w;

    let index = i32(look.y);
    let col = f32(index % i32(columns));
    let row = f32(index / i32(columns));
    let uv = vec2<f32>((col * cell + in.local.x * cell) / atlasWidth,
                       (row * cell + (1.0 - in.local.y) * cell) / atlasHeight);
    let sampled = textureSampleLevel(fontTexture, fontSampler, uv, 0.0);

    let fill = sampled.r;
    let outline = sampled.g;
    let together = max(fill, outline);
    if (together <= 0.02) {
        discard;
    }

    // The outline is black and the fill is the caller's colour, which is what
    // keeps a count legible over a bright icon.
    let ink = mix(vec3<f32>(0.0, 0.0, 0.0), tint.rgb, fill);
    let alpha = together * tint.a;
    return vec4<f32>(ink * alpha, alpha);
}
)";
} // namespace mh
