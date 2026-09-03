#pragma once

// The box a dead character gets, and the two answers it takes.
//
// FFXI does not let a corpse do anything except choose: go to the home point,
// or wait for someone to raise you. This client used to say so as three lines
// of chat, which puts the one decision the game insists on in the same place
// as everything else scrolling past, and offers no way to make it except by
// typing a command at a character who cannot move.
//
// Drawn the way the HUD and the nameplates are: one triangle over the whole
// window, everything else in a uniform block, and the fragment shader working
// out what each pixel belongs to. No vertex buffer and one draw call for the
// panel, the buttons and the text together - a modal box is a handful of
// rectangles, and a rectangle is two comparisons to test against.
//
// Where each glyph sits is worked out on the CPU, as it is for the nameplates
// and for the same reason: the font is proportional, so a letter's position
// depends on every letter before it, and a fragment shader cannot accumulate
// that per pixel without re-walking the whole string.

namespace mh
{
/// How many rows the box holds. A form is a title, a message, a row per
/// field and button, and - when a choice is unfolded - a row per option under
/// it; character creation with its options open needs the most.
inline constexpr int kDialogRows = 32;

/// Characters per row. Long enough for a sentence at this width.
inline constexpr int kDialogChars = 40;

/// How many of those rows are buttons. The last ones, always.
inline constexpr int kDialogButtons = 2;

/// The WGSL below spells these out - a shader cannot see a C++ constant - so
/// they are asserted rather than trusted. Getting them out of step resizes the
/// uniform buffer without changing what the shader reads, which shows up as
/// garbage rows rather than as an error.
static_assert(kDialogRows == 32, "dialog_shader.h WGSL hardcodes 32 rows");
static_assert(kDialogChars == 40, "dialog_shader.h WGSL hardcodes kChars = 40");

inline constexpr const char* kDialogShader = R"(
struct DialogUniforms {
    // Rows in use, one atlas cell in NDC y, the aspect, and how far the world
    // behind the box is dimmed.
    counts : vec4<f32>,
    // Atlas shape: columns, cell size, width, height.
    atlas : vec4<f32>,
    // The box itself: left, bottom, width, height, in NDC.
    panel : vec4<f32>,
    // The text caret in the focused field: left, bottom, width, height. Width
    // zero means no field has focus and nothing is drawn.
    caret : vec4<f32>,
    // Per row, the button behind it: left, bottom, width, height. A width of
    // zero is a row of plain text with nothing drawn behind it.
    rects : array<vec4<f32>, 32>,
    // Per row, that button's fill: colour, then how opaque it is.
    fills : array<vec4<f32>, 32>,
    // Per row, the text: left and bottom in NDC, width in cells, unused.
    boxes : array<vec4<f32>, 32>,
    // Per row, the text's colour, then its size against the shared cell.
    colours : array<vec4<f32>, 32>,
    // Per glyph: atlas cell index, x offset in cells, advance in cells.
    // Laid out row-major, kDialogChars apart.
    glyphs : array<vec4<f32>, 1280>,
};

const kChars = 40;

@group(0) @binding(0) var<uniform> dialog : DialogUniforms;
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
    let rows = i32(dialog.counts.x);
    if (rows <= 0) {
        discard;
    }

    let cellBase = dialog.counts.y;
    let aspect = dialog.counts.z;

    let columns = dialog.atlas.x;
    let cell = dialog.atlas.y;
    let atlasWidth = dialog.atlas.z;
    let atlasHeight = dialog.atlas.w;

    // The world behind the box goes down rather than away. A modal that hid
    // the zone would hide the person walking over to raise you, which is the
    // one thing worth watching for while this is up.
    var colour = vec3<f32>(0.0, 0.0, 0.0);
    var alpha = dialog.counts.w;

    // An even border needs the horizontal inset divided by the aspect, or a
    // wide window draws a thin frame down the sides and a thick one across.
    let edge = 0.0045;
    let edgeX = edge / aspect;

    let panel = dialog.panel;
    if (in.ndc.x >= panel.x && in.ndc.x < panel.x + panel.z &&
        in.ndc.y >= panel.y && in.ndc.y < panel.y + panel.w) {
        let onEdge = in.ndc.x < panel.x + edgeX || in.ndc.x >= panel.x + panel.z - edgeX ||
                     in.ndc.y < panel.y + edge || in.ndc.y >= panel.y + panel.w - edge;
        colour = select(vec3<f32>(0.05, 0.06, 0.09), vec3<f32>(0.62, 0.68, 0.82), onEdge);
        alpha = select(0.93, 1.0, onEdge);
    }

    // The last opaque rectangle over this fragment. Rows are drawn in order,
    // so a later rectangle sits on an earlier one - an unfolded choice's
    // options hang over the rows beneath - and the text of anything under it
    // must not show through.
    var covering = -1;

    for (var row = 0; row < rows; row = row + 1) {
        let rect = dialog.rects[row];
        if (rect.z <= 0.0) {
            continue;
        }
        if (in.ndc.x < rect.x || in.ndc.x >= rect.x + rect.z ||
            in.ndc.y < rect.y || in.ndc.y >= rect.y + rect.w) {
            continue;
        }

        let fill = dialog.fills[row];
        if (fill.a >= 0.999) {
            covering = row;
        }
        let onEdge = in.ndc.x < rect.x + edgeX || in.ndc.x >= rect.x + rect.z - edgeX ||
                     in.ndc.y < rect.y + edge || in.ndc.y >= rect.y + rect.w - edge;
        colour = select(fill.rgb, mix(fill.rgb, vec3<f32>(1.0, 1.0, 1.0), 0.45), onEdge);
        alpha = max(alpha, fill.a);
    }

    let caret = dialog.caret;
    if (caret.z > 0.0 &&
        in.ndc.x >= caret.x && in.ndc.x < caret.x + caret.z &&
        in.ndc.y >= caret.y && in.ndc.y < caret.y + caret.w) {
        colour = vec3<f32>(0.98, 0.98, 1.0);
        alpha = 1.0;
    }

    for (var row = 0; row < rows; row = row + 1) {
        if (row < covering) {
            continue;   // under something solid drawn after it
        }
        let text = dialog.boxes[row];
        let widthCells = text.z;
        if (widthCells <= 0.0) {
            continue;
        }

        let scale = dialog.colours[row].w;
        let cellHigh = cellBase * scale;
        let cellWide = cellHigh / aspect;
        let width = widthCells * cellWide;

        // A cell of slack on the right, because the last glyph reaches past
        // where the pen stopped - see hud_shader.h, which had the same fault.
        let local = vec2<f32>(in.ndc.x - text.x, in.ndc.y - text.y);
        if (local.x < 0.0 || local.x >= width + cellWide || local.y < 0.0 || local.y >= cellHigh) {
            continue;
        }

        // Which glyph the fragment is over. Cells overlap, so this takes the
        // strongest coverage of any of them rather than stopping at the first.
        let xCells = local.x / cellWide;
        var ink = 0.0;
        var outline = 0.0;
        for (var i = 0; i < kChars; i = i + 1) {
            let glyph = dialog.glyphs[row * kChars + i];
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
            let atlasRow = f32(index / i32(columns));
            let uv = vec2<f32>((col * cell + insideX * cell) / atlasWidth,
                               (atlasRow * cell + insideY * cell) / atlasHeight);
            let sampled = textureSampleLevel(fontTexture, fontSampler, uv, 0.0);
            ink = max(ink, sampled.r);
            outline = max(outline, sampled.g);
        }

        // Blended into what is already there rather than replacing it. The
        // HUD's text has nothing behind it and can afford to overwrite; this
        // sits on a solid panel, and a glyph that overwrote its own soft edge
        // would be cut out of the panel rather than drawn on it.
        let together = max(ink, outline);
        if (together > 0.02) {
            colour = mix(colour, mix(vec3<f32>(0.0, 0.0, 0.0), dialog.colours[row].rgb, ink), together);
            alpha = max(alpha, together);
        }
    }

    return vec4<f32>(colour, alpha);
}
)";
} // namespace mh
