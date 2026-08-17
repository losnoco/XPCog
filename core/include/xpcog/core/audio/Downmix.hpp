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
// Only the reducing direction is implemented. Cog also upmixes -- stereo into a
// surround layout -- which is a separate function there and a separate question
// here, since it only matters once XPCog negotiates a device channel count that
// exceeds the source's.

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

}  // namespace xpcog
