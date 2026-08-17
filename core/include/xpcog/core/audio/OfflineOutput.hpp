// A capturing IAudioOutput for tests and offline rendering.
//
// Exists so the gapless seam test can run without an audio device -- CI has none,
// and a seam needs to be inspected sample by sample rather than listened to.

#pragma once

#include <memory>
#include <vector>

namespace xpcog {

class IAudioOutput;
class RingBuffer;

/// Drains `sink` as fast as the feeder fills it, capturing everything.
///
/// `speedMultiple` optionally paces that drain: 0 (the default) consumes with no
/// rate limit at all, while a positive value consumes at that multiple of real
/// time, so 8.0 plays eight seconds of audio per wall-clock second.
///
/// Unlimited is right for anything checking *what* was produced -- a seam, a
/// resampler null test -- and keeps those tests fast and free of timing flake.
/// It is wrong for anything that has to observe playback while it is still under
/// way: a short file drains in the time it takes to decode it, so a test that
/// plays one and then acts on it is racing the drain thread and will lose on a
/// fast enough machine. Pacing makes that window a known quantity instead. Keep
/// the multiple high enough that the test stays quick and low enough that the
/// window comfortably exceeds the poll interval.
[[nodiscard]] std::unique_ptr<IAudioOutput> makeOfflineOutput(RingBuffer& sink,
                                                              double speedMultiple = 0.0);

/// Everything captured so far, interleaved float32. Returns empty for an output
/// that is not an offline one.
[[nodiscard]] std::vector<float> capturedAudio(const IAudioOutput& output);

}  // namespace xpcog
