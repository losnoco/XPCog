// Captures golden reference output from Cog's FreeSurround decoder.
//
// Why this exists at all: FreeSurround is a matrix surround decoder, stereo to
// 5.1, and Cog's copy is built on vDSP -- a double-precision real DFT, plus
// vDSP's windowing, packing and accumulate. Porting it means replacing every one
// of those with our own arithmetic, and unlike the equaliser there is no
// closed-form response to check the result against. A wrong twiddle sign or a
// half-block offset does not fail; it moves the sound field slightly, which
// nobody hears until they compare. So the reference is captured from Cog while
// Cog's kernel is still the thing running, and the port is held to it.
//
// Deliberately outside the CMake build. It compiles against a Cog checkout
// rather than against anything in this tree, it only builds on macOS because
// vDSP only exists there, and it is run when the artwork of the algorithm
// changes -- which is approximately never. Making it a build target would put a
// Cog checkout on the dependency list of every build machine to produce a file
// that is committed. See README.md for the command.
//
// The fixture it writes is the *kernel's* output, in FreeSurround's own channel
// order, before any mapping to a WFX layout. That mapping is our own code and
// belongs in our own tests; what cannot be re-derived is the arithmetic.

#include "freesurround_decoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr unsigned kBlockSize  = 4096;
constexpr unsigned kBlocks     = 8;
constexpr double   kSampleRate = 44100.0;

// Numerical Recipes' LCG. The generator is part of the fixture: the committed
// file is only meaningful next to the input that produced it, and storing the
// input as well would double a 768 KiB fixture to say something eleven lines of
// code already say exactly.
uint32_t g_lcg = 20260817u;

/// [-1, 1) from the top 24 bits.
///
/// The scale is a power of two, so the multiply is exact, and the fixture
/// therefore reproduces bit-for-bit on any IEEE-754 platform. That matters more
/// than it looks: this file is captured on macOS and compared against on
/// Windows and Linux, so an input that merely *nearly* reproduces would show up
/// as the port being wrong.
float nextNoise() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(g_lcg >> 8) - 8388608) * (1.0F / 8388608.0F);
}

/// One stereo relationship per block, so every steering regime is exercised and
/// each one is attributable to a block when a comparison fails.
///
/// Every coefficient here is a power of two or a negation. Nothing rounds, so
/// no difference in fma contraction between two compilers can make one
/// platform's *input* differ from another's -- which would otherwise be
/// indistinguishable from the port being wrong.
///
/// The regimes bleed into each other by design rather than by accident: the
/// decoder overlap-adds at half a block, so output block N mixes input blocks
/// N-1 and N, and the transitions are covered as well as the steady states.
void fillBlock(unsigned block, float* out) {
    for (unsigned i = 0; i < kBlockSize; ++i) {
        const float n = nextNoise();
        float       l = n;
        float       r = n;
        switch (block % 8) {
            case 0: r = n; break;            // mono -- steers hard to centre
            case 1: r = nextNoise(); break;  // uncorrelated -- diffuse
            case 2: r = -n; break;           // anti-phase -- steers hard to rear
            case 3: r = 0.5F * n; break;     // panned left
            case 4: l = 0.5F * n; break;     // panned right
            case 5: r = nextNoise(); break;  // uncorrelated, different state
            case 6: r = -0.5F * n; break;    // part anti-phase
            case 7: r = n; break;            // mono, different state
            default: break;
        }
        out[i * 2]     = l;
        out[i * 2 + 1] = r;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* stem = (argc > 1) ? argv[1] : "fsurround-5point1";

    freesurround_decoder decoder(cs_5point1, kBlockSize);

    // Cog's shipping values, from freesurround_params in FSurroundFilter.mm.
    // Not the decoder's own defaults, and the difference is the point: this is
    // the configuration Cog actually runs, so it is the one worth pinning.
    //
    // bass_redirection is false there, which leaves LFE silent for the whole
    // capture. That is faithful, and it does mean the bass-redirection path is
    // not covered by this fixture -- see README.md.
    decoder.circular_wrap(90.0F);
    decoder.shift(0.0F);
    decoder.depth(1.0F);
    decoder.focus(0.0F);
    decoder.center_image(0.7F);
    decoder.front_separation(1.0F);
    decoder.rear_separation(1.0F);
    decoder.bass_redirection(false);
    decoder.low_cutoff(static_cast<float>(40.0 / (kSampleRate / 2.0)));
    decoder.high_cutoff(static_cast<float>(90.0 / (kSampleRate / 2.0)));

    const unsigned channels = freesurround_decoder::num_channels(cs_5point1);

    char path[512];
    std::snprintf(path, sizeof(path), "%s.f32", stem);
    std::FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        std::perror("fopen");
        return 1;
    }

    std::snprintf(path, sizeof(path), "%s.txt", stem);
    std::FILE* manifest = std::fopen(path, "w");
    if (manifest == nullptr) {
        std::perror("fopen");
        std::fclose(out);
        return 1;
    }

    std::fprintf(manifest, "# Golden reference: Cog's FreeSurround decoder, cs_5point1.\n");
    std::fprintf(manifest, "# Generated by tools/fsurround-golden/capture.cpp. Do not edit.\n");
    std::fprintf(manifest, "blocksize %u\nblocks %u\nchannels %u\nsamplerate %.0f\n",
                 kBlockSize, kBlocks, channels, kSampleRate);
    std::fprintf(manifest, "layout");
    for (unsigned c = 0; c < channels; ++c) {
        std::fprintf(manifest, " 0x%x",
                     static_cast<unsigned>(freesurround_decoder::channel_at(cs_5point1, c)));
    }
    std::fprintf(manifest, "\nlcg_seed %u\n", 20260817u);

    std::vector<float> input(static_cast<std::size_t>(kBlockSize) * 2);
    for (unsigned b = 0; b < kBlocks; ++b) {
        fillBlock(b, input.data());
        const float* decoded = decoder.decode(input.data());
        std::fwrite(decoded, sizeof(float), static_cast<std::size_t>(kBlockSize) * channels, out);

        // Per-channel RMS, so a human can see at a glance that the capture is
        // the shape it should be -- centre loud on the mono blocks, rears loud
        // on the anti-phase ones -- without opening the binary.
        std::fprintf(manifest, "rms %u", b);
        std::printf("block %u rms:", b);
        for (unsigned c = 0; c < channels; ++c) {
            double sum = 0.0;
            for (unsigned i = 0; i < kBlockSize; ++i) {
                const double v = decoded[i * channels + c];
                sum += v * v;
            }
            const double rms = std::sqrt(sum / kBlockSize);
            std::fprintf(manifest, " %.6f", rms);
            std::printf(" %.6f", rms);
        }
        std::fprintf(manifest, "\n");
        std::printf("\n");
    }

    std::fclose(manifest);
    std::fclose(out);
    std::printf("wrote %s.f32 and %s.txt\n", stem, stem);
    return 0;
}
