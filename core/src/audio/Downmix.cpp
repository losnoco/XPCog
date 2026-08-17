#include "xpcog/core/audio/Downmix.hpp"

#include "xpcog/core/AudioFormat.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xpcog {
namespace {

/// Every ratio the stereo matrix needs, for one channel config.
struct Ratios {
    float front[2]   = {0.0F, 0.0F};
    float frontCentre = 0.0F;
    float lfe         = 0.0F;
    float back[2]     = {0.0F, 0.0F};
    float backCentre  = 0.0F;
    float side[2]     = {0.0F, 0.0F};
};

/// Cog's ladder, in Cog's order. The order matters: several steps scale what the
/// previous ones set, and backCentre is derived from frontCentre *after* it has
/// already been attenuated, so reordering these changes the result.
[[nodiscard]] Ratios ratiosFor(std::uint32_t config) {
    Ratios r;

    if ((config & (kChannelFrontLeft | kChannelFrontRight)) != 0U) {
        r.front[0] = 1.0F;
    }
    if ((config & kChannelFrontCenter) != 0U) {
        r.front[0]    = 0.5858F;
        r.frontCentre = 0.4142F;
    }
    if ((config & (kChannelBackLeft | kChannelBackRight)) != 0U) {
        if ((config & kChannelFrontCenter) != 0U) {
            r.front[0]    = 0.651F;
            r.frontCentre = 0.46F;
            r.back[0]     = 0.5636F;
            r.back[1]     = 0.3254F;
        } else {
            r.front[0] = 0.4226F;
            r.back[0]  = 0.366F;
            r.back[1]  = 0.2114F;
        }
    }
    if ((config & kChannelLFE) != 0U) {
        r.front[0] *= 0.8F;
        r.frontCentre *= 0.8F;
        r.lfe = r.frontCentre;
        r.back[0] *= 0.8F;
        r.back[1] *= 0.8F;
    }
    if ((config & kChannelBackCenter) != 0U) {
        r.front[0] *= 0.86F;
        r.frontCentre *= 0.86F;
        r.lfe *= 0.86F;
        r.back[0] *= 0.86F;
        r.back[1] *= 0.86F;
        r.backCentre = r.frontCentre * 0.86F;
    }
    if ((config & (kChannelSideLeft | kChannelSideRight)) != 0U) {
        const float ratio = ((config & kChannelBackCenter) != 0U) ? 0.85F : 0.73F;
        r.front[0] *= ratio;
        r.frontCentre *= ratio;
        r.lfe *= ratio;
        r.back[0] *= ratio;
        r.back[1] *= ratio;
        r.backCentre *= ratio;
        r.side[0] = 0.463882352941176F * ratio;
        r.side[1] = 0.267882352941176F * ratio;
    }

    return r;
}

/// The left and right ratio for one canonical channel index. The indexes are bit
/// positions in ChannelFlag, which is the same numbering Cog's findChannelIndex
/// returns -- 0 front left, 3 LFE, 8 back centre, and so on.
void ratioForIndex(const Ratios& r, std::uint32_t index, float& left, float& right) {
    left  = 0.0F;
    right = 0.0F;

    switch (index) {
        case 0:  // front left
            left  = r.front[0];
            right = r.front[1];
            break;
        case 1:  // front right -- mirrored, so the pair does not cross-mix
            left  = r.front[1];
            right = r.front[0];
            break;
        case 2:  // front centre
            left  = r.frontCentre;
            right = r.frontCentre;
            break;
        case 3:  // LFE
            left  = r.lfe;
            right = r.lfe;
            break;
        case 4:  // back left
            left  = r.back[0];
            right = r.back[1];
            break;
        case 5:  // back right
            left  = r.back[1];
            right = r.back[0];
            break;
        case 8:  // back centre
            left  = r.backCentre;
            right = r.backCentre;
            break;
        case 9:  // side left
            left  = r.side[0];
            right = r.side[1];
            break;
        case 10:  // side right
            left  = r.side[1];
            right = r.side[0];
            break;
        default:
            // Front centre left/right (6, 7) and everything from the top layer
            // up (11 and above) are dropped, exactly as they are in Cog.
            break;
    }
}

}  // namespace

void downmixToStereo(const float* in, std::uint32_t inChannels, std::uint32_t config,
                     float* out, std::size_t frames) {
    if (in == nullptr || out == nullptr || inChannels == 0) {
        return;
    }
    if (config == 0U) {
        config = guessChannelConfig(inChannels);
    }

    const Ratios ratios = ratiosFor(config);

    std::fill_n(out, frames * 2, 0.0F);

    for (std::uint32_t channel = 0; channel < inChannels; ++channel) {
        const std::uint32_t flag = extractChannelFlag(channel, config);
        if (flag == 0U) {
            continue;  // fewer flags in the config than channels in the buffer
        }

        float left  = 0.0F;
        float right = 0.0F;
        ratioForIndex(ratios, static_cast<std::uint32_t>(std::countr_zero(flag)), left,
                      right);
        if (left == 0.0F && right == 0.0F) {
            continue;
        }

        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float sample = in[(frame * inChannels) + channel];
            out[frame * 2] += sample * left;
            out[(frame * 2) + 1] += sample * right;
        }
    }
}

void downmixToMono(const float* in, std::uint32_t inChannels, std::uint32_t config,
                   float* out, std::size_t frames) {
    if (in == nullptr || out == nullptr || inChannels == 0) {
        return;
    }

    // Via stereo, as Cog does, so the matrix is applied once and mono is simply
    // its average rather than a second set of constants to keep in step.
    std::vector<float> stereo(frames * 2);
    downmixToStereo(in, inChannels, config, stereo.data(), frames);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        out[frame] = 0.5F * (stereo[frame * 2] + stereo[(frame * 2) + 1]);
    }
}

}  // namespace xpcog
