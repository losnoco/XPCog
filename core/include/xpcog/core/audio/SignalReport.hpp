// What happens to the audio between the decoder and the device, as facts. Port
// of what Cog's OutputStatus carries (Audio/Engine/Facade/OutputStatus.swift,
// 0997c57cb), for the Signal Inspector.
//
// Numbers and flags, no words: core has no catalogue, so uicore's SignalChain
// turns this into the stages a listener reads. It describes the track being
// heard rather than the one being decoded, which a gapless engine is a buffer
// ahead of, and it is assembled off the real-time thread: the output callback
// knows nothing of it.

#pragma once

#include "xpcog/core/AudioFormat.hpp"

#include <cstdint>
#include <string>

namespace xpcog {

struct SignalReport {
    /// The track as its decoder produces it.
    struct Source {
        AudioFormat format;
        std::string codec;
        std::string encoding;
        bool        lossless = false;
        /// ReplayGain or volume scaling, as a linear factor.
        float trackGain = 1.0F;
    };

    /// The chain's settings as last applied on the DSP thread.
    struct Dsp {
        bool        equalizer = false;  ///< on, and not flat
        double      preamp    = 0.0;
        bool        fades     = false;
        bool        stretch   = false;  ///< tempo or pitch away from unity
        double      tempo     = 1.0;
        double      pitch     = 1.0;
    };

    /// False while stopped, when nothing else here means anything.
    bool active = false;
    bool paused = false;

    Source source;
    Dsp    dsp;

    bool        hdcdDecoded  = false;  ///< HDCD was found in this track, and is being decoded
    bool        freeSurround = false;
    std::string resampleQuality;

    /// What everything is converted to and the device runs.
    AudioFormat device;
    bool        exclusive = false;
    float       volume    = 1.0F;

    double        latencySeconds  = 0.0;
    double        bufferedSeconds = 0.0;
    std::uint64_t underruns       = 0;
};

}  // namespace xpcog
