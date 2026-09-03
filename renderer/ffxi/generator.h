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
#include <unordered_map>
#include <utility>
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
    /// Op 0x27 was present. Only Bastok Markets' `allsea` and `lowsea` carry
    /// it: a sheet scaled a hundredfold to span the whole zone at every
    /// level, sitting just under each floor. Drawn, it covered the auction
    /// house floor. Read as "not drawn directly" - a reflection or water-table
    /// plane - until the opcode is understood.
    bool hidden{};
    /// Op 0x48: four distances. Fades in between the first two and out
    /// between the last two, so a lamp's big soft halo (`lglt`: 10, 50, 100,
    /// 150; the fountain's `llit`: 50, 70, 150, 180) shows only from afar
    /// and is not a wall of light when standing under the lamp. Zero when
    /// absent, meaning always shown.
    float fade[4]{};
};

/// A value over the Vana'diel day, from a type 0x19 chunk: pairs of (time as
/// a fraction of the day, value), the last pairs sometimes (0, 0) padding.
///
/// This is the day/night switch. `frtm`, the fountain flames' curve, is 0.78
/// from midnight to 06:12, zero from 06:48 to 17:12, then 0.78 again; `watm`,
/// the jets', is its inverse; `ksta`, the stars', is 1.34 at midnight and
/// zero from 05:36 to 17:36. A generator names its curve with op 0x63 (or
/// 0x60 on an untextured model), the same field this project first read as
/// a "texture animation" - the stream's `tkwa` never reaches zero, it only
/// dims the water through the day.
struct IntensityCurve
{
    std::vector<std::pair<float, float>> keys;
    /// Linear between keys; the first or last value beyond them.
    float at(float dayFraction) const;
};

/// Every 0x19 chunk in the file, by its four-character id. Ids repeat across
/// a DAT's directories (`k000` appears in each weather); the last one read
/// wins, and the copies seen so far agree.
std::unordered_map<std::string, IntensityCurve> parseIntensityCurves(const DatFile& dat);

/// Every generator in the file that places a model.
///
/// Generators that place nothing - particle emitters with no model, sound
/// triggers - are left out. Nothing here throws: a chunk that does not parse
/// is skipped.
std::vector<EffectPlacement> parseGenerators(const DatFile& dat);
} // namespace ffxi
