// The Signal Inspector's words.

#include "SignalChain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace xpcog;
using xpcog::app::describeSignal;
using xpcog::app::SignalChain;
using xpcog::app::SignalStage;

namespace {

SignalReport cdOnACdRateDevice() {
    SignalReport report;
    report.active                      = true;
    report.source.format.sampleRate    = 44100.0;
    report.source.format.channels      = 2;
    report.source.format.bitsPerSample = 16;
    report.source.codec                = "FLAC";
    report.device.sampleRate           = 44100.0;
    report.device.channels             = 2;
    report.device.bitsPerSample        = 32;
    report.resampleQuality             = "high";
    return report;
}

const SignalStage& stage(const SignalChain& chain, const std::string& name) {
    const auto found = std::find_if(chain.stages.begin(), chain.stages.end(),
                                    [&name](const SignalStage& s) { return s.name == name; });
    REQUIRE(found != chain.stages.end());
    return *found;
}

}  // namespace

TEST_CASE("nothing playing has no stages", "[uicore][signal]") {
    const SignalChain chain = describeSignal(SignalReport{});
    CHECK_FALSE(chain.active);
    CHECK(chain.stages.empty());
    CHECK(chain.summary == "Nothing playing");
}

TEST_CASE("a track nothing touches is called unchanged", "[uicore][signal]") {
    const SignalChain chain = describeSignal(cdOnACdRateDevice());
    CHECK(chain.active);
    CHECK(chain.unchanged);
    CHECK(chain.summary.starts_with("Unchanged"));
    CHECK(stage(chain, "Resampler").state == SignalStage::State::Off);
    CHECK(stage(chain, "Decoder").detail.starts_with("FLAC"));
    CHECK(chain.live.size() == 3);
}

TEST_CASE("every stage that changes the audio says so", "[uicore][signal]") {
    SignalReport report = cdOnACdRateDevice();

    SECTION("resampling") {
        report.device.sampleRate = 48000.0;
        const SignalChain chain  = describeSignal(report);
        CHECK(stage(chain, "Resampler").state == SignalStage::State::Changing);
        CHECK_FALSE(chain.unchanged);
    }
    SECTION("track gain") {
        report.source.trackGain = 0.5F;
        const SignalChain chain = describeSignal(report);
        CHECK(stage(chain, "Track gain").detail == "-6.02 dB");
        CHECK_FALSE(chain.unchanged);
    }
    SECTION("the equaliser") {
        report.dsp.equalizer = true;
        CHECK_FALSE(describeSignal(report).unchanged);
    }
    SECTION("volume") {
        report.volume = 0.5F;
        CHECK(stage(describeSignal(report), "Volume").detail == "50%");
    }
    SECTION("fitting channels") {
        report.source.format.channels = 6;
        CHECK(stage(describeSignal(report), "Channels").state == SignalStage::State::Changing);
    }
    SECTION("FreeSurround says so once, not as a channel change too") {
        report.freeSurround    = true;
        report.device.channels = 6;
        const SignalChain chain = describeSignal(report);
        CHECK(stage(chain, "FreeSurround").state == SignalStage::State::Changing);
        CHECK(stage(chain, "Channels").state == SignalStage::State::Passing);
    }
}
