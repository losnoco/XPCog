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
[[nodiscard]] std::unique_ptr<IAudioOutput> makeOfflineOutput(RingBuffer& sink);

/// Everything captured so far, interleaved float32. Returns empty for an output
/// that is not an offline one.
[[nodiscard]] std::vector<float> capturedAudio(const IAudioOutput& output);

}  // namespace xpcog
