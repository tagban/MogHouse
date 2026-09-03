#pragma once

// Effect generators - the type 0x05 chunks of a zone DAT.
//
// The MZB's placement table places the terrain and the buildings. Everything
// that moves or shimmers is placed by the effect system instead: each type
// 0x05 chunk is a generator that names a model (by the four-character id of
// its MMB chunk), says where it goes, and how it is turned and scaled. A
// zone's water is placed this way - East Ronfaure's stream is thirty-six
// hand-built surfaces, ka1..ka22 and kb1..kb14, each set on a forty-unit grid
// by its own generator - which is why the water models were in every zone's
// DAT and "nothing referenced them". See docs/generator-format.md.

#include "dat.h"

#include <string>
#include <vector>

namespace ffxi
{
struct EffectPlacement
{
    std::string generator;        ///< the chunk's own four-character id
    /// The directory chunks enclosing it, joined with '/': "t_ba/effe/funs"
    /// for Bastok Markets' fountain, "t_ba/weat/fine/star" for its stars.
    /// The weather directories place the sky - the moon, the stars, the
    /// clouds - and those are not scenery to put through the effect pass.
    std::string directory;
    std::string modelId;          ///< four-character id of the MMB chunk it places
    float translate[3]{};         ///< DAT frame, same as an MZB placement
    float rotate[3]{};            ///< radians
    float scale[3]{1.0f, 1.0f, 1.0f};
    std::string textureAnimation; ///< e.g. "tkwa", the scrolling river sheet; may be empty
    /// Op 0x0d was present. Bastok Markets' fountain flames and its night
    /// glow carry it; the jets, the basin and East Ronfaure's stream do not.
    /// Read as "night only" and gated on the clock - a reading, not yet a
    /// confirmed one.
    bool nightOnly{};
    /// Op 0x28: how far the texture slides per frame, in uv. -0.007 on the
    /// fountain jets, 0.003 on the stream; zero when absent.
    float scroll{};
};

/// Every generator in the file that places a model.
///
/// Generators that place nothing - particle emitters with no model, sound
/// triggers - are left out. Nothing here throws: a chunk that does not parse
/// is skipped.
std::vector<EffectPlacement> parseGenerators(const DatFile& dat);
} // namespace ffxi
