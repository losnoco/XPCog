// Choosing an equaliser preset, as opposed to writing its curve.
//
// applyEqualizerPreset() writes 31 gains and a preamp. That is not the whole of
// what choosing a preset means, and the difference is what made a preset applied
// over the REST API look like it did nothing: a curve is inaudible while the
// equaliser is switched off, so picking "Rock" from a phone changed 31 stored
// numbers and not one audible thing.
//
// The window had always got this right -- EqualizerPanel::selectPreset switches
// the equaliser on and says why, "picking 'Bass Booster' and hearing nothing is
// the same trap" -- and the API had not. These functions are that policy in one
// place, so there is no longer a second front end to get it wrong.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

struct Fixture {
    std::unique_ptr<ISettingsStore> store = makeMemorySettingsStore();
    Settings                        settings{*store};

    [[nodiscard]] std::vector<double> gains() {
        std::vector<double> values;
        for (const char* key : Equalizer::bandSettingsKeys()) {
            const std::string raw = settings.rawValue(key);
            values.push_back(raw.empty() ? 0.0 : std::stod(raw));
        }
        return values;
    }
};

}  // namespace

TEST_CASE("choosing a preset switches the equaliser on", "[dsp][remote]") {
    Fixture fixture;
    REQUIRE_FALSE(fixture.settings.GraphicEqEnable());

    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Rock"));

    // The bug in one line: without this the curve was stored and nothing was
    // audible.
    CHECK(fixture.settings.GraphicEqEnable());

    const std::vector<double> gains = fixture.gains();
    REQUIRE(gains.size() == 31);
    CHECK(gains.front() != 0.0);
}

TEST_CASE("Flat does not switch the equaliser on", "[dsp][remote]") {
    Fixture fixture;

    // Flat is what somebody reaches for to *stop* hearing the equaliser, so
    // switching it on to deliver a curve that does nothing would be perverse.
    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Flat"));
    CHECK_FALSE(fixture.settings.GraphicEqEnable());

    for (const double gain : fixture.gains()) {
        CHECK(gain == 0.0);
    }
}

TEST_CASE("Flat leaves an equaliser that was already on alone", "[dsp][remote]") {
    Fixture fixture;

    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Rock"));
    REQUIRE(fixture.settings.GraphicEqEnable());

    // It declines to switch the equaliser *on*. It does not switch it off.
    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Flat"));
    CHECK(fixture.settings.GraphicEqEnable());
}

TEST_CASE("the chosen preset is recorded and reads back by name", "[dsp][remote]") {
    Fixture fixture;

    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Jazz"));

    // Recorded, so the window's dropdown agrees with what a remote client did
    // rather than still naming whatever was chosen before it.
    CHECK(fixture.settings.GraphicEqPreset() ==
          shippedEqualizerPresets().indexOf("Jazz"));
    CHECK(equalizerPresetName(fixture.settings) == "Jazz");
}

TEST_CASE("a curve is no preset once it is marked custom", "[dsp][remote]") {
    Fixture fixture;

    REQUIRE(applyEqualizerPresetByName(fixture.settings, "Rock"));
    REQUIRE(equalizerPresetName(fixture.settings) == "Rock");

    markEqualizerCustom(fixture.settings);

    // Saying it was still Rock would be a dropdown naming a curve that is not
    // the one in the sliders.
    CHECK(equalizerPresetName(fixture.settings).empty());
    // The gains themselves are left where they are: custom means "not a preset",
    // not "reset".
    CHECK(fixture.gains().front() != 0.0);
}

TEST_CASE("no preset is named before one is chosen", "[dsp][remote]") {
    Fixture fixture;
    // The default is -1, which says "not a preset" and keeps saying it.
    CHECK(equalizerPresetName(fixture.settings).empty());
}

TEST_CASE("an unknown preset changes nothing", "[dsp][remote]") {
    Fixture fixture;

    CHECK_FALSE(applyEqualizerPresetByName(fixture.settings, "Nonesuch"));
    CHECK_FALSE(fixture.settings.GraphicEqEnable());
    CHECK(equalizerPresetName(fixture.settings).empty());
    for (const double gain : fixture.gains()) {
        CHECK(gain == 0.0);
    }
}
