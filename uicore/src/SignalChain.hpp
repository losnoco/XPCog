// The path from the decoder to the device, in words: core's SignalReport as the
// stages the Signal Inspector lists, and the line under them. Port of Cog's
// SignalChain (SignalInspector/SignalStatusModel.swift), in the layer with a
// catalogue, so both players say the same thing.
//
// Cog can say "Bit perfect" because it holds the device in integer mode. XPCog
// hands the system float and the system's mixer may do anything after that, so
// the verdict here claims only what it can see: that XPCog left the samples
// alone, or did not.

#pragma once

#include "xpcog/core/audio/SignalReport.hpp"

#include <string>
#include <vector>

namespace xpcog::app {

struct SignalStage {
    enum class State {
        Passing,   ///< in the path, leaving the samples as they were
        Changing,  ///< in the path, changing them
        Off,       ///< not in use, for the reason `detail` gives
    };

    std::string name;
    State       state = State::Passing;
    std::string detail;
};

struct SignalChain {
    /// False while nothing plays, when the rest is empty.
    bool active = false;

    /// XPCog passed the decoded samples on unchanged.
    bool unchanged = false;

    /// "Unchanged · 44.1 kHz · 2 channels", for one line.
    std::string summary;

    std::vector<SignalStage> stages;

    /// Latency, how much is queued, and underruns, one per line.
    std::vector<std::string> live;
};

[[nodiscard]] SignalChain describeSignal(const SignalReport& report);

}  // namespace xpcog::app
