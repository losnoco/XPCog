// The FreeSurround port, against Cog's own output.
//
// This is the only check this kernel gets, and the reason is worth restating:
// the decoder is a fitted polynomial steering a 21x21 gain surface, so there is
// no closed form to test it against the way the equaliser has one. What there is
// instead is tests/golden/fsurround-5point1.f32 -- eight blocks captured from
// Cog while Cog's vDSP kernel was still the thing running. See
// tools/fsurround-golden/.
//
// The input is regenerated here rather than stored. That is safe because the
// generator uses only an integer LCG and power-of-two coefficients, so it
// reproduces bit-for-bit off the Mac it was captured on -- if it did not, a
// mismatch here would be indistinguishable from the port being wrong.

#include "xpcog/core/audio/FreeSurround.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using xpcog::FreeSurround;

namespace {

constexpr unsigned kBlockSize  = 4096;
constexpr unsigned kBlocks     = 8;
constexpr unsigned kChannels   = 6;
constexpr double   kSampleRate = 44100.0;

/// Byte-for-byte the generator in tools/fsurround-golden/capture.cpp.
class GoldenInput {
public:
    void fill(unsigned block, float* out) {
        for (unsigned i = 0; i < kBlockSize; ++i) {
            const float n = next();
            float       l = n;
            float       r = n;
            switch (block % 8) {
                case 0: r = n; break;
                case 1: r = next(); break;
                case 2: r = -n; break;
                case 3: r = 0.5F * n; break;
                case 4: l = 0.5F * n; break;
                case 5: r = next(); break;
                case 6: r = -0.5F * n; break;
                case 7: r = n; break;
                default: break;
            }
            out[i * 2]     = l;
            out[i * 2 + 1] = r;
        }
    }

private:
    float next() {
        state_ = state_ * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<std::int32_t>(state_ >> 8) - 8388608) *
               (1.0F / 8388608.0F);
    }

    std::uint32_t state_ = 20260817U;
};

std::vector<float> readGolden() {
    const std::string path = std::string(XPCOG_GOLDEN_DIR) + "/fsurround-5point1.f32";
    std::FILE*        file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);

    std::vector<float> data(static_cast<std::size_t>(kBlocks) * kBlockSize * kChannels);
    const std::size_t  read = std::fread(data.data(), sizeof(float), data.size(), file);
    std::fclose(file);
    REQUIRE(read == data.size());
    return data;
}

/// Cog's settings, from FSurroundFilter.mm. Applied to a reference rather than
/// returned by value: the decoder owns buffers and is deliberately non-copyable,
/// and giving it a move constructor to please a test helper would be the test
/// shaping the class.
void applyCogSettings(FreeSurround& decoder) {
    decoder.setCircularWrap(90.0F);
    decoder.setShift(0.0F);
    decoder.setDepth(1.0F);
    decoder.setFocus(0.0F);
    decoder.setCenterImage(0.7F);
    decoder.setFrontSeparation(1.0F);
    decoder.setRearSeparation(1.0F);
    decoder.setBassRedirection(false);
    decoder.setLowCutoff(static_cast<float>(40.0 / (kSampleRate / 2.0)));
    decoder.setHighCutoff(static_cast<float>(90.0 / (kSampleRate / 2.0)));
}

}  // namespace

TEST_CASE("the port reproduces Cog's FreeSurround output", "[audio][fsurround]") {
    const std::vector<float> golden = readGolden();

    FreeSurround decoder(cs_5point1, kBlockSize);
    applyCogSettings(decoder);
    REQUIRE(decoder.channels() == kChannels);

    GoldenInput        input;
    std::vector<float> block(static_cast<std::size_t>(kBlockSize) * 2);

    // Absolute rather than relative, because the quantity compared is a sample
    // and a relative tolerance says nothing near a zero crossing.
    //
    // The measured worst case across the whole fixture is 4.8e-7, against peaks
    // of about 2.6 -- two ULP of float32 at that magnitude. In other words the
    // port agrees with Cog as closely as single precision can express, and what
    // is left is the difference between vDSP's rounding and RealFft's. The
    // threshold is set just above that rather than at a comfortable round
    // number. A gross error -- the scale factor, a swapped channel -- fails at
    // any tolerance; what a loose one hides is a drift of a few ULP, which is
    // the signature of the arithmetic having genuinely diverged somewhere rather
    // than merely rounded differently. 1e-6 leaves room for the observed
    // rounding and very little else.
    constexpr double kTolerance = 1.0e-6;

    for (unsigned b = 0; b < kBlocks; ++b) {
        input.fill(b, block.data());
        const float* out = decoder.decode(block.data());

        double worst      = 0.0;
        std::size_t where = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kBlockSize) * kChannels; ++i) {
            const double diff = std::abs(
                static_cast<double>(out[i]) -
                static_cast<double>(golden[(static_cast<std::size_t>(b) * kBlockSize * kChannels) + i]));
            if (diff > worst) {
                worst = diff;
                where = i;
            }
        }
        INFO("block " << b << " worst at frame " << (where / kChannels) << " channel "
                      << (where % kChannels) << ", delta " << worst);
        REQUIRE(worst < kTolerance);
    }
}

TEST_CASE("flush drops the overlap history", "[audio][fsurround]") {
    // A seek must not smear half a block of the old position into the new one.
    // Checked by feeding the same first block twice: once from a fresh decoder,
    // once from one that has been run and then flushed. Only the overlap-add
    // history distinguishes them.
    GoldenInput        firstInput;
    std::vector<float> first(static_cast<std::size_t>(kBlockSize) * 2);
    firstInput.fill(0, first.data());

    FreeSurround fresh(cs_5point1, kBlockSize);
    applyCogSettings(fresh);
    const float* expected = fresh.decode(first.data());
    const std::vector<float> reference(
        expected, expected + static_cast<std::size_t>(kBlockSize) * kChannels);

    FreeSurround used(cs_5point1, kBlockSize);
    applyCogSettings(used);
    GoldenInput  other;
    std::vector<float> noise(static_cast<std::size_t>(kBlockSize) * 2);
    other.fill(2, noise.data());
    used.decode(noise.data());
    CHECK(used.buffered() == kBlockSize / 2);

    used.flush();
    CHECK(used.buffered() == 0);

    const float* after = used.decode(first.data());
    for (std::size_t i = 0; i < reference.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(static_cast<double>(after[i]) ==
                Catch::Approx(static_cast<double>(reference[i])).margin(1e-9));
    }
}
