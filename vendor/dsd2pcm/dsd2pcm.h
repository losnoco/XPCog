// DSD to PCM: an eight-to-one decimating low-pass, one instance per channel.
//
// Sebastian Gesemann's filter, taken from Cog's Audio/Chain/ChunkList.m where it
// sits inline rather than as a library. Vendored rather than made an overlay
// port by the rule in ../../ports/README.md -- it is a single file with no
// upstream in this form, like `lpc.c` and `hdcd_decode2.c` beside it.
//
// What it is for: DSD is one bit at a very high rate, and a bit is not a sample
// anything downstream can use. A `.wv` holding DSD128 reports 705,600 Hz and
// eight bits per byte -- 5.6 MHz of one-bit audio -- and playing those bytes as
// if they were PCM produces noise at a rate no device will accept. This turns
// each byte into one 32-bit float, so DSD64 comes out at 352,800 Hz and DSD128
// at 705,600 Hz, and the resampler downstream does the rest.
//
// The filter is the second half of an even-order symmetric FIR: passband to
// 24 kHz with 0.025 dB of ripple, stopband from 176.4 kHz at 170 dB. That
// rejection is the whole point -- DSD's noise shaping pushes its quantisation
// noise up above the audio band, and a gentler filter would fold it back in.
//
// The 64 coefficients are applied through a lookup table rather than a multiply
// per tap: every input bit is +1 or -1, so eight of them at a time index a
// 256-entry table of pre-summed contributions. Eight tables, 8 KB, and the inner
// loop is eight table reads and adds per output sample instead of sixty-four
// multiplies.

#ifndef XPCOG_DSD2PCM_H
#define XPCOG_DSD2PCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsd2pcm_state dsd2pcm_state;

/// One channel's filter, ready to run. NULL if out of memory.
dsd2pcm_state *dsd2pcm_alloc(void);

void dsd2pcm_free(dsd2pcm_state *state);

/// Back to silence. The FIFO is filled with 0x55/0xAA rather than zeroes: a DSD
/// zero is alternating bits, and filling with actual zeroes would start every
/// stream with a step down to negative full scale.
void dsd2pcm_reset(dsd2pcm_state *state);

/// How many PCM samples of delay the filter adds, for whoever reports latency.
int dsd2pcm_latency(const dsd2pcm_state *state);

/// Turns `len` bytes of one channel into `len` floats.
///
/// The offset-and-stride pair on each side is what lets one call walk a channel
/// of an interleaved buffer, which is how both sides arrive: `sinc` and `dinc`
/// are the channel count, `sofs` and `dofs` the channel's index.
void dsd2pcm_process(dsd2pcm_state *state, const uint8_t *src, size_t sofs,
                     size_t sinc, float *dest, size_t dofs, size_t dinc,
                     size_t len);

#ifdef __cplusplus
}
#endif

#endif
