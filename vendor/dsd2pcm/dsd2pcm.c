// See dsd2pcm.h. The algorithm and its coefficients are Sebastian Gesemann's,
// by way of Cog's Audio/Chain/ChunkList.m; what changed here is that it is a
// file with an interface rather than 200 lines inside a buffer class, and that
// the state struct is opaque. dsd2pcm_dup() is not carried across -- Cog needs
// it to copy a chunk list, and nothing here copies a filter.

#include "dsd2pcm.h"

#include <stdlib.h>
#include <string.h>

/**
 * This is the 2nd half of an even order symmetric FIR
 * lowpass filter (to be used on a signal sampled at 44100*64 Hz)
 * Passband is 0-24 kHz (ripples +/- 0.025 dB)
 * Stopband starts at 176.4 kHz (rejection: 170 dB)
 * The overall gain is 2.0
 */
#define DSD2PCM_FILTER_COEFFS_COUNT 64
static const float DSD2PCM_FILTER_COEFFS[64] = {
	0.09712411121659f, 0.09613438994044f, 0.09417884216316f, 0.09130441727307f,
	0.08757947648990f, 0.08309142055179f, 0.07794369263673f, 0.07225228745463f,
	0.06614191680338f, 0.05974199351302f, 0.05318259916599f, 0.04659059631228f,
	0.04008603356890f, 0.03377897290478f, 0.02776684382775f, 0.02213240062966f,
	0.01694232798846f, 0.01224650881275f, 0.00807793792573f, 0.00445323755944f,
	0.00137370697215f, -0.00117318019994f, -0.00321193033831f, -0.00477694265140f,
	-0.00591028841335f, -0.00665946056286f, -0.00707518873201f, -0.00720940203988f,
	-0.00711340642819f, -0.00683632603227f, -0.00642384017266f, -0.00591723006715f,
	-0.00535273320457f, -0.00476118922548f, -0.00416794965654f, -0.00359301524813f,
	-0.00305135909510f, -0.00255339111833f, -0.00210551956895f, -0.00171076760278f,
	-0.00136940723130f, -0.00107957856005f, -0.00083786862365f, -0.00063983084245f,
	-0.00048043272086f, -0.00035442550015f, -0.00025663481039f, -0.00018217573430f,
	-0.00012659899635f, -0.00008597726991f, -0.00005694188820f, -0.00003668060332f,
	-0.00002290670286f, -0.00001380895679f, -0.00000799057558f, -0.00000440385083f,
	-0.00000228567089f, -0.00000109760778f, -0.00000047286430f, -0.00000017129652f,
	-0.00000004282776f, 0.00000000119422f, 0.00000000949179f, 0.00000000747450f
};

struct dsd2pcm_state {
	/* Constant for the life of the filter */
	int      lookupParts;
	float   *lookupTable;
	uint8_t *reverseBits;
	int      fifoLength;
	int      fifoMask;

	/* Altered as it runs */
	int *fifo;
	int  fpos;
};

dsd2pcm_state *dsd2pcm_alloc(void) {
	struct dsd2pcm_state *state =
	    (struct dsd2pcm_state *)calloc(1, sizeof(struct dsd2pcm_state));
	double *temp = NULL;

	if(!state)
		return NULL;

	state->lookupParts = (DSD2PCM_FILTER_COEFFS_COUNT + 7) / 8;

	/* One table per eight taps, 256 entries each: the sum of those eight
	 * coefficients for every possible arrangement of the eight bits, added
	 * where the bit is set and subtracted where it is not. */
	state->lookupTable = (float *)calloc((size_t)state->lookupParts << 8, sizeof(float));
	if(!state->lookupTable)
		goto fail;

	temp = (double *)calloc(0x100, sizeof(double));
	if(!temp)
		goto fail;

	for(int part = 0, sofs = 0, dofs = 0; part < state->lookupParts;) {
		memset(temp, 0, 0x100 * sizeof(double));
		for(int bit = 0, bitmask = 0x80;
		    bit < 8 && sofs + bit < DSD2PCM_FILTER_COEFFS_COUNT;) {
			const double coeff = DSD2PCM_FILTER_COEFFS[sofs + bit];
			for(int bite = 0; bite < 0x100; bite++) {
				if((bite & bitmask) == 0) {
					temp[bite] -= coeff;
				} else {
					temp[bite] += coeff;
				}
			}
			bit++;
			bitmask >>= 1;
		}
		for(int s = 0; s < 0x100;) {
			state->lookupTable[dofs++] = (float)temp[s++];
		}
		part++;
		sofs += 8;
	}
	free(temp);
	temp = NULL;

	/* The FIFO holds both halves of the symmetric filter and is indexed with
	 * a mask, so its length is rounded up to a power of two. */
	{
		int k = 1;
		while(k < state->lookupParts * 2) {
			k <<= 1;
		}
		state->fifoLength = k;
		state->fifoMask   = k - 1;
	}

	/* The second half of a symmetric filter reads its bytes backwards, so the
	 * bit order is reversed by table rather than per byte. */
	state->reverseBits = (uint8_t *)calloc(1, 0x100);
	if(!state->reverseBits)
		goto fail;
	for(int i = 0, j = 0; i < 0x100; i++) {
		state->reverseBits[i] = (uint8_t)j;
		/* "reverse-increment" of j */
		for(int bitmask = 0x80;;) {
			if(((j ^= bitmask) & bitmask) != 0)
				break;
			if(bitmask == 1)
				break;
			bitmask >>= 1;
		}
	}

	state->fifo = (int *)calloc((size_t)state->fifoLength, sizeof(int));
	if(!state->fifo)
		goto fail;

	dsd2pcm_reset(state);
	return state;

fail:
	free(temp);
	dsd2pcm_free(state);
	return NULL;
}

void dsd2pcm_free(dsd2pcm_state *state) {
	if(state) {
		free(state->fifo);
		free(state->reverseBits);
		free(state->lookupTable);
		free(state);
	}
}

void dsd2pcm_reset(dsd2pcm_state *state) {
	if(!state)
		return;
	for(int i = 0; i < state->lookupParts; i++) {
		state->fifo[i]                      = 0x55;
		state->fifo[i + state->lookupParts] = 0xAA;
	}
	state->fpos = state->lookupParts;
}

int dsd2pcm_latency(const dsd2pcm_state *state) {
	return state ? state->lookupParts * 8 : 0;
}

void dsd2pcm_process(dsd2pcm_state *state, const uint8_t *src, size_t sofs,
                     size_t sinc, float *dest, size_t dofs, size_t dinc,
                     size_t len) {
	if(!state)
		return;

	int           *fifo        = state->fifo;
	const uint8_t *reverseBits = state->reverseBits;
	const float   *lookupTable = state->lookupTable;
	const int      lookupParts = state->lookupParts;
	const int      fifoMask    = state->fifoMask;
	int            fpos        = state->fpos;

	while(len > 0) {
		fifo[fpos]                                 = reverseBits[fifo[fpos]] & 0xFF;
		fifo[(fpos + lookupParts) & fifoMask]      = src[sofs] & 0xFF;
		sofs += sinc;

		const int temp   = (fpos + 1) & fifoMask;
		float     sample = 0;
		for(int k = 0, lofs = 0; k < lookupParts;) {
			const int bite1 = fifo[(fpos - k) & fifoMask];
			const int bite2 = fifo[(temp + k) & fifoMask];
			sample += lookupTable[lofs + bite1] + lookupTable[lofs + bite2];
			k++;
			lofs += 0x100;
		}
		fpos = temp;

		dest[dofs] = sample;
		dofs += dinc;
		len--;
	}

	state->fpos = fpos;
}
