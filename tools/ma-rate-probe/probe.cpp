// Does miniaudio resample behind our back?
//
// It does, when the rate asked for is not one the backend will actually run.
// This reports the two numbers that say so, for one device, at the rates XPCog
// plays at. See tools/ma-rate-probe/README.md for what the output means and
// what was found with it.
//
// Deliberately talks to miniaudio directly rather than through IAudioOutput:
// the whole question is about a field IAudioOutput does not expose, and a probe
// that went through the seam could only tell you what the seam already says.

#include <miniaudio.h>

#include <cstdio>
#include <initializer_list>

namespace {

/// Silence. The probe is about negotiation, not about audio, and a device that
/// is opened and started should not also make a noise.
void callback(ma_device*, void* output, const void*, ma_uint32 frames) {
    ma_silence_pcm_frames(output, frames, ma_format_f32, 2);
}

void probe(ma_uint32 rate, ma_share_mode mode) {
    const char* modeName = (mode == ma_share_mode_exclusive) ? "exclusive" : "shared";

    ma_device_config config  = ma_device_config_init(ma_device_type_playback);
    config.playback.format    = ma_format_f32;
    config.playback.channels  = 2;
    config.playback.shareMode = mode;
    config.sampleRate         = rate;
    config.dataCallback       = callback;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        std::printf("  %6u Hz %-9s : would not open\n", rate, modeName);
        return;
    }

    // device.sampleRate is what the *caller* gets to think the rate is;
    // playback.internalSampleRate is what the hardware is running. miniaudio
    // inserts a data converter between them, and its default resampler is
    // linear -- ma_device_config_init() sets ma_resample_algorithm_linear and
    // XPCog has never overridden it.
    const bool converting = device.sampleRate != device.playback.internalSampleRate;
    std::printf("  %6u Hz %-9s : device=%-6u internal=%-6u  %s\n", rate, modeName,
                device.sampleRate, device.playback.internalSampleRate,
                converting ? "*** miniaudio is resampling (linear) ***"
                           : "no conversion");

    ma_device_uninit(&device);
}

}  // namespace

int main() {
    std::printf("default playback device, f32 stereo\n\nshared:\n");
    for (const ma_uint32 rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        probe(rate, ma_share_mode_shared);
    }
    std::printf("\nexclusive:\n");
    for (const ma_uint32 rate : {44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
        probe(rate, ma_share_mode_exclusive);
    }
    return 0;
}
