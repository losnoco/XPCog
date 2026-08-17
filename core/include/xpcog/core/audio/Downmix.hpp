// Matrix downmix. Port of Cog Audio/Chain/DSP/Downmix.m.
//
// Not a DSPNode, which is a deliberate departure from Cog and worth explaining.
// Cog's nodes pass AudioChunks that carry their own format, so a node can change
// the channel count; XPCog's DSPNode transforms a buffer in place at a fixed
// format (see DSPNode.hpp), and downmix by definition changes the frame size. So
// it belongs where channel geometry is already changing -- AudioConverter, next to
// resampling -- rather than being forced into a contract that cannot express it.
// The placeholder it replaces in AudioConverter said "DSPDownmixNode's job in M4";
// this is that job, in the place M4 turned out to want it.
//
// The ratios are Cog's, constant for constant. They are not derived from
// anything documented there either -- 0.5858/0.4142 for front-plus-centre is the
// familiar -3 dB centre split, but 0.463882352941176 for the sides is a magic
// number in Cog too, and it is reproduced rather than rationalised so that a
// 5.1 file mixes to exactly what Cog would give.
//
// The growing direction is routing, not a matrix: each input channel goes to the
// output slot carrying the same flag, and slots with no source stay silent. Cog's
// upmix() is a ladder of per-layout cases, but every non-mono case reduces to
// exactly that routing -- plus one genuine rule, splitting a back centre into
// back left and right when the target has no BC slot -- so the port keeps the
// rules and drops the ladder.
//
// One Cog bug fixed rather than reproduced, in the house tradition: its 6.1 case
// reads the side pair at interleave indexes 4-5 and back centre at 6, but 6.1 in
// flag order is FL FR FC LFE *BC SL SR* (BC is bit 8, the sides bits 9-10, in
// Cog's own AudioChunk.h), so upmixing a 6.1 source rotates three channels --
// back centre plays from a side speaker and a side channel lands on BC or the
// back pair. Routing by flag cannot express that mistake.

#pragma once

#include <cstddef>
#include <cstdint>

namespace xpcog {

/// Mixes `inChannels` interleaved channels laid out per `config` down to
/// interleaved stereo. `out` holds `frames * 2` samples and is overwritten.
///
/// Channels the matrix has no place for -- height, top, front-centre-left and
/// friends -- contribute nothing, which is Cog's behaviour: they fall through its
/// switch to a zero ratio.
void downmixToStereo(const float* in, std::uint32_t inChannels, std::uint32_t config,
                     float* out, std::size_t frames);

/// As above, then averages the two into one channel. `out` holds `frames`.
void downmixToMono(const float* in, std::uint32_t inChannels, std::uint32_t config,
                   float* out, std::size_t frames);

/// Routes `inChannels` laid out per `inConfig` into `outChannels` laid out per
/// `outConfig`. `out` holds `frames * outChannels` and is overwritten; slots with
/// no source are silent.
///
/// Two rules beyond flag matching, both Cog's. A mono source headed for a layout
/// with no centre slot duplicates into front left and right instead of
/// vanishing. And a back centre headed for a layout without one splits into back
/// left and right, so 6.1 material keeps its rear image on a 7.1 bed.
void upmix(const float* in, std::uint32_t inChannels, std::uint32_t inConfig,
           float* out, std::uint32_t outChannels, std::uint32_t outConfig,
           std::size_t frames);

}  // namespace xpcog
