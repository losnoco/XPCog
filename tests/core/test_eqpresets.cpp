// The equaliser's preset library: the file format, the interpolation, and the
// genre match.
//
// The interpolation is the part worth testing hardest, and for an unusual
// reason: it is the only place in the equaliser where the *right* answer is not
// derivable from first principles. A biquad either matches its transfer
// function or it does not, and test_dsp.cpp checks that two ways. A preset's
// curve is whatever Cog's interpolatePoint() says it is -- so what these tests
// pin is not "correct" but "the same", and the expectations below are computed
// by hand from Cog's formula rather than read off this implementation.
//
// The extrapolated bands (20, 25, 31.5 and 20000 Hz) get their own case for the
// same reason. They are the four of the 31 that no preset stores a value for,
// they are produced by a formula that looks like a mistake until it is read
// twice, and nothing about a wrong answer there would sound wrong -- it would
// just be a slightly different bass shelf.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

using namespace xpcog;
using Catch::Approx;

namespace {

/// A library with one preset, written the way the file writes it: gains as
/// integers in tenths of a dB offset by 201.
[[nodiscard]] std::string documentWith(const std::string& body) {
    return R"({"type":"Cog EQ library file v1.0","presets":[)" + body + "]}";
}

[[nodiscard]] std::string presetJson(const std::string& name, int hz32, int hz64,
                                     int hz128, int hz256, int hz512, int hz1000,
                                     int hz2000, int hz4000, int hz8000, int hz16000,
                                     int preamp, const std::string& extra = {}) {
    return R"({"name":")" + name + R"(","hz32":)" + std::to_string(hz32) +
           R"(,"hz64":)" + std::to_string(hz64) + R"(,"hz128":)" +
           std::to_string(hz128) + R"(,"hz256":)" + std::to_string(hz256) +
           R"(,"hz512":)" + std::to_string(hz512) + R"(,"hz1000":)" +
           std::to_string(hz1000) + R"(,"hz2000":)" + std::to_string(hz2000) +
           R"(,"hz4000":)" + std::to_string(hz4000) + R"(,"hz8000":)" +
           std::to_string(hz8000) + R"(,"hz16000":)" + std::to_string(hz16000) +
           R"(,"preamp":)" + std::to_string(preamp) + extra + "}";
}

/// dB expressed the way the file expresses it.
[[nodiscard]] int stored(double decibels) {
    return static_cast<int>(std::lround(decibels * 10.0)) + 201;
}

/// The index of a band by its centre frequency, so a test can say "the 1 kHz
/// band" without counting.
[[nodiscard]] std::size_t bandAt(double hertz) {
    const auto centres = Equalizer::bandFrequencies();
    const auto it      = std::find(centres.begin(), centres.end(), hertz);
    REQUIRE(it != centres.end());
    return static_cast<std::size_t>(it - centres.begin());
}

}  // namespace

TEST_CASE("the shipped library is the one Cog ships", "[eqpresets]") {
    const auto& library = shippedEqualizerPresets();

    // Loaded through assetPath(), so a failure here is as likely to be the
    // staging rule as the parser -- which is worth saying, because the test
    // binary finding no asset is the shape that mistake takes.
    REQUIRE_FALSE(library.empty());
    CHECK(library.size() == 22);

    CHECK(library.indexOf("Flat") >= 0);
    CHECK(library.indexOf("Bass Booster") >= 0);
    CHECK(library.indexOf("Vocal Booster") >= 0);
    CHECK(library.indexOf("No Such Preset") == -1);

    const EqualizerPreset* flat = library.at(library.indexOf("Flat"));
    REQUIRE(flat != nullptr);
    CHECK(flat->name == "Flat");
}

TEST_CASE("Flat is flat at every one of the 31 centres", "[eqpresets]") {
    // Not a tautology: Flat stores ten zeroes, and the four extrapolated bands
    // are computed rather than copied. A sign error in the extension would leave
    // Flat flat only by luck -- it survives because zero minus zero is zero --
    // so this pins the one preset whose curve has an obvious right answer.
    const auto& library = shippedEqualizerPresets();
    REQUIRE_FALSE(library.empty());
    const EqualizerPreset* flat = library.at(library.indexOf("Flat"));
    REQUIRE(flat != nullptr);

    CHECK(flat->preampDb == Approx(0.0));
    for (const double gain : interpolateEqualizerPreset(*flat)) {
        CHECK(gain == Approx(0.0).margin(1e-12));
    }
}

TEST_CASE("a stored gain is tenths of a dB offset by 201", "[eqpresets]") {
    const auto library = EqualizerPresetLibrary::parse(documentWith(
        presetJson("Scale", 1, 401, 201, 202, 200, 251, 151, 201, 201, 201, 301)));
    REQUIRE(library.size() == 1);
    const EqualizerPreset& preset = *library.at(0);

    CHECK(preset.gainsDb[0] == Approx(-20.0));  // 1
    CHECK(preset.gainsDb[1] == Approx(20.0));   // 401
    CHECK(preset.gainsDb[2] == Approx(0.0));    // 201
    CHECK(preset.gainsDb[3] == Approx(0.1));    // 202
    CHECK(preset.gainsDb[4] == Approx(-0.1));   // 200
    CHECK(preset.gainsDb[5] == Approx(5.0));    // 251
    CHECK(preset.gainsDb[6] == Approx(-5.0));   // 151
    CHECK(preset.preampDb == Approx(10.0));     // 301
}

TEST_CASE("a gain outside the stored range reads as 0 dB, not as the rail",
          "[eqpresets]") {
    // Cog's rule, and the less obvious of the two possibilities: a value this
    // far out means the file was written against a different scale, and
    // flattening one band is a smaller misreading than pinning it to +20.
    const auto library = EqualizerPresetLibrary::parse(documentWith(
        presetJson("Out Of Range", 0, 402, -5, 100000, 100, 201, 201, 201, 201, 201,
                   201)));
    REQUIRE(library.size() == 1);
    const EqualizerPreset& preset = *library.at(0);

    CHECK(preset.gainsDb[0] == Approx(0.0));
    CHECK(preset.gainsDb[1] == Approx(0.0));
    CHECK(preset.gainsDb[2] == Approx(0.0));
    CHECK(preset.gainsDb[3] == Approx(0.0));
    // 100 is inside the range, and is the control: the four beside it read as
    // 0 dB because they were rejected, not because rejection is all this does.
    CHECK(preset.gainsDb[4] == Approx(-10.1));
}

TEST_CASE("the five shared centres are copied, not interpolated", "[eqpresets]") {
    // 1000, 2000, 4000, 8000 and 16000 Hz are in both band tables, so a preset's
    // value at them must arrive unchanged. The other five stored points (32 to
    // 512 Hz) have no equivalent centre and cannot be checked this way.
    const auto library = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Spikes", 201, 201, 201, 201, 201, stored(6.0),
                                stored(-3.0), stored(9.0), stored(-7.0), stored(2.0),
                                201)));
    REQUIRE(library.size() == 1);
    const auto gains = interpolateEqualizerPreset(*library.at(0));

    CHECK(gains[bandAt(1000.0)] == Approx(6.0));
    CHECK(gains[bandAt(2000.0)] == Approx(-3.0));
    CHECK(gains[bandAt(4000.0)] == Approx(9.0));
    CHECK(gains[bandAt(8000.0)] == Approx(-7.0));
    CHECK(gains[bandAt(16000.0)] == Approx(2.0));
}

TEST_CASE("a centre between two stored points is their linear blend",
          "[eqpresets]") {
    // 500 Hz sits between the 256 Hz and 512 Hz points, at (500-256)/(512-256) =
    // 0.953125 of the way across. With 0 dB at 256 and 10 dB at 512 that is
    // 9.53125 dB, computed from Cog's formula rather than from this one.
    const auto library = EqualizerPresetLibrary::parse(documentWith(presetJson(
        "Ramp", 201, 201, 201, 201, stored(10.0), 201, 201, 201, 201, 201, 201)));
    REQUIRE(library.size() == 1);
    const auto gains = interpolateEqualizerPreset(*library.at(0));

    CHECK(gains[bandAt(500.0)] == Approx(9.53125));

    // 400 Hz is in the same interval, and further from the top of it.
    CHECK(gains[bandAt(400.0)] == Approx((400.0 - 256.0) / 256.0 * 10.0));
}

TEST_CASE("the four centres no preset stores continue the outermost segment",
          "[eqpresets]") {
    // Cog builds four synthetic points beyond each end, stepping by 1.05 in gain
    // and frequency together, and interpolates inside them. Those points lie on
    // the line through the two outermost stored points, so the 1.05 cancels: what
    // comes out is the outermost segment continued straight, and the scale factor
    // could be anything positive without moving a band.
    //
    // So the expectation here is the closed form -- computed from the two stored
    // points and nothing else -- rather than a replay of the loop. That is the
    // same tactic test_dsp.cpp uses on the cascade, and for the same reason: an
    // expectation derived the way the code derives it can only prove the code
    // agrees with itself.
    const auto library = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Slope", stored(6.0), stored(2.0), 201, 201, 201, 201,
                                201, 201, stored(1.0), stored(3.0), 201)));
    REQUIRE(library.size() == 1);
    const auto gains = interpolateEqualizerPreset(*library.at(0));

    // Below 32 Hz: the line through (64 Hz, 2 dB) and (32 Hz, 6 dB), extended.
    const auto belowThirtyTwo = [](double hertz) {
        return 6.0 + (hertz - 32.0) * ((6.0 - 2.0) / (32.0 - 64.0));
    };
    CHECK(gains[bandAt(20.0)] == Approx(belowThirtyTwo(20.0)));
    CHECK(gains[bandAt(25.0)] == Approx(belowThirtyTwo(25.0)));
    CHECK(gains[bandAt(31.5)] == Approx(belowThirtyTwo(31.5)));

    // Above 16 kHz: the line through (8 kHz, 1 dB) and (16 kHz, 3 dB), extended.
    const auto aboveSixteenK = [](double hertz) {
        return 3.0 + (hertz - 16000.0) * ((3.0 - 1.0) / (16000.0 - 8000.0));
    };
    CHECK(gains[bandAt(20000.0)] == Approx(aboveSixteenK(20000.0)));

    // Spelled out, so the numbers are legible without evaluating a lambda: a
    // 4 dB rise over the top octave becomes 4.0 dB at 20 kHz, and a 4 dB rise
    // from 64 to 32 Hz keeps rising to 7.5 dB at 20 Hz.
    CHECK(gains[bandAt(20.0)] == Approx(7.5));
    CHECK(gains[bandAt(20000.0)] == Approx(4.0));
}

TEST_CASE("the extrapolated bands follow the slope's direction", "[eqpresets]") {
    // The property behind the arithmetic above, stated so a future change to the
    // formula has to break something legible: a preset that falls towards 32 Hz
    // keeps falling below it, and one that rises keeps rising.
    const auto falling = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Falling", stored(0.0), stored(6.0), 201, 201, 201,
                                201, 201, 201, 201, 201, 201)));
    const auto rising = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Rising", stored(6.0), stored(0.0), 201, 201, 201, 201,
                                201, 201, 201, 201, 201)));
    REQUIRE(falling.size() == 1);
    REQUIRE(rising.size() == 1);

    const auto fell = interpolateEqualizerPreset(*falling.at(0));
    const auto rose = interpolateEqualizerPreset(*rising.at(0));

    CHECK(fell[bandAt(20.0)] < fell[bandAt(25.0)]);
    CHECK(fell[bandAt(25.0)] < fell[bandAt(31.5)]);
    CHECK(rose[bandAt(20.0)] > rose[bandAt(25.0)]);
    CHECK(rose[bandAt(25.0)] > rose[bandAt(31.5)]);
}

TEST_CASE("applying a preset writes the preamp and all 31 bands", "[eqpresets]") {
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const auto library = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Loud", stored(6.0), stored(4.0), 201, 201, stored(-2.0),
                                201, stored(-1.0), stored(-5.0), stored(5.0),
                                stored(1.0), stored(-3.0))));
    REQUIRE(library.size() == 1);
    const EqualizerPreset& preset = *library.at(0);

    applyEqualizerPreset(settings, preset);

    CHECK(settings.EqPreamp() == Approx(-3.0));

    const auto gains = interpolateEqualizerPreset(preset);
    const auto keys  = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        INFO("band " << keys[band]);
        CHECK(std::stod(settings.rawValue(keys[band])) == Approx(gains[band]).margin(1e-6));
    }

    // The point of writing settings rather than the filter: the equaliser that
    // reads them afterwards produces the preset's curve without knowing a preset
    // exists.
    Equalizer equalizer;
    equalizer.setBandGains(gains);
    CHECK(equalizer.active());
}

TEST_CASE("a genre picks the preset Cog would pick", "[eqpresets]") {
    const auto& library = shippedEqualizerPresets();
    REQUIRE_FALSE(library.empty());

    const int flat = library.indexOf("Flat");
    REQUIRE(flat >= 0);

    SECTION("an exact name matches") {
        CHECK(library.matchGenre("Jazz") == library.indexOf("Jazz"));
        CHECK(library.matchGenre("Classical") == library.indexOf("Classical"));
    }

    SECTION("a name inside the genre matches, ignoring case") {
        CHECK(library.matchGenre("Progressive Rock") == library.indexOf("Rock"));
        CHECK(library.matchGenre("dance") == library.indexOf("Dance"));
        CHECK(library.matchGenre("Electronic Body Music") ==
              library.indexOf("Electronic"));
    }

    SECTION("nothing recognisable falls back to Flat") {
        CHECK(library.matchGenre("Shibuya-kei") == flat);
        CHECK(library.matchGenre("!!!") == flat);
    }

    SECTION("an untagged track flattens, which is the part worth knowing") {
        // Cog's behaviour, and the reason genre tracking is off by default: this
        // is not "leave it alone", it is "flatten it".
        CHECK(library.matchGenre("") == flat);
    }
}

TEST_CASE("the longest matching name wins, not the first", "[eqpresets]") {
    const auto library = EqualizerPresetLibrary::parse(
        documentWith(presetJson("Rock", 201, 201, 201, 201, 201, 201, 201, 201, 201,
                                201, 201) +
                     "," +
                     presetJson("Punk Rock", 201, 201, 201, 201, 201, 201, 201, 201,
                                201, 201, 201)));
    REQUIRE(library.size() == 2);

    // "Rock" is inside "Punk Rock" and comes first in the file. The specific
    // preset has to win anyway, or every compound genre collapses onto whichever
    // short name happens to be listed earliest.
    CHECK(library.matchGenre("Punk Rock") == 1);
    CHECK(library.matchGenre("Gothic Punk Rock Revival") == 1);
    CHECK(library.matchGenre("Post-Rock") == 0);
}

TEST_CASE("altGenres are alternate names for a preset", "[eqpresets]") {
    // Cog's parser reads the alias array with the wrong index and so never
    // actually loads one; the shipped library declares none, which is why nobody
    // noticed. This reads them properly, so a library that uses the feature works
    // here.
    const auto library = EqualizerPresetLibrary::parse(documentWith(presetJson(
        "Hip-Hop", 201, 201, 201, 201, 201, 201, 201, 201, 201, 201, 201,
        R"(,"altGenres":["Rap","Trap"])")));
    REQUIRE(library.size() == 1);
    CHECK(library.at(0)->altGenres.size() == 2);

    CHECK(library.indexOf("Rap") == 0);
    CHECK(library.matchGenre("Trap") == 0);
    CHECK(library.matchGenre("Dirty South Rap") == 0);
}

TEST_CASE("the two indices that mean no preset resolve to nothing", "[eqpresets]") {
    const auto& library = shippedEqualizerPresets();
    REQUIRE_FALSE(library.empty());

    // -1 is the default: a curve that was never a preset. size() is the Custom
    // row at the end of the selector, which is what moving a slider sets. Both
    // have to answer nullptr rather than wrap onto a real preset.
    CHECK(library.at(-1) == nullptr);
    CHECK(library.at(static_cast<int>(library.size())) == nullptr);
    CHECK(library.at(9999) == nullptr);
    CHECK(library.at(0) != nullptr);
}

TEST_CASE("a document that is not a preset library yields nothing", "[eqpresets]") {
    // Every one of these has the same answer -- an empty library -- because
    // every caller's response to a broken library is the response to a missing
    // one: leave the curve alone.
    CHECK(EqualizerPresetLibrary::parse("").empty());
    CHECK(EqualizerPresetLibrary::parse("not json at all").empty());
    CHECK(EqualizerPresetLibrary::parse("{").empty());
    CHECK(EqualizerPresetLibrary::parse("[]").empty());
    CHECK(EqualizerPresetLibrary::parse(R"({"presets":[]})").empty());
    CHECK(EqualizerPresetLibrary::parse(
              R"({"type":"Some Other Format","presets":[]})")
              .empty());
    CHECK(EqualizerPresetLibrary::parse(
              R"({"type":"Cog EQ library file v1.0","presets":{}})")
              .empty());

    // Trailing junk fails rather than parsing as the valid prefix, so a
    // truncated download cannot read as a shorter library.
    CHECK(EqualizerPresetLibrary::parse(
              documentWith(presetJson("Flat", 201, 201, 201, 201, 201, 201, 201, 201,
                                      201, 201, 201)) +
              "trailing")
              .empty());
}

TEST_CASE("a preset missing a member is dropped and the rest are kept",
          "[eqpresets]") {
    const std::string good = presetJson("Good", 201, 201, 201, 201, 201, 201, 201, 201,
                                        201, 201, 201);
    // No "preamp", which is one of the twelve required members.
    const std::string missing =
        R"({"name":"Missing","hz32":201,"hz64":201,"hz128":201,"hz256":201,)"
        R"("hz512":201,"hz1000":201,"hz2000":201,"hz4000":201,"hz8000":201,)"
        R"("hz16000":201})";
    // A gain written as a real rather than an integer, which Cog's type check
    // rejects -- so this implementation rejects it too, deliberately.
    const std::string real = presetJson("Real", 201, 201, 201, 201, 201, 201, 201, 201,
                                        201, 201, 201);

    const auto library =
        EqualizerPresetLibrary::parse(documentWith(missing + "," + good));
    REQUIRE(library.size() == 1);
    CHECK(library.at(0)->name == "Good");

    std::string realGain = real;
    realGain.replace(realGain.find(R"("hz32":201)"), std::string(R"("hz32":201)").size(),
                     R"("hz32":251.0)");
    const auto rejected = EqualizerPresetLibrary::parse(documentWith(realGain));
    CHECK(rejected.empty());
}

TEST_CASE("the library survives a name that is not ASCII", "[eqpresets]") {
    // The file XPCog ships is ASCII; one a user writes need not be, and a name
    // that arrives mangled is the hardest kind to retype into a settings file.
    const auto escaped = EqualizerPresetLibrary::parse(documentWith(
        presetJson(R"(Ambi\u00e9nt)", 201, 201, 201, 201, 201, 201, 201, 201, 201,
                   201, 201)));
    REQUIRE(escaped.size() == 1);
    CHECK(escaped.at(0)->name == "Ambi\xC3\xA9nt");

    // A surrogate pair, which is two escapes for one code point.
    const auto pair = EqualizerPresetLibrary::parse(documentWith(presetJson(
        R"(\ud83c\udfb5 Bass)", 201, 201, 201, 201, 201, 201, 201, 201, 201, 201,
        201)));
    REQUIRE(pair.size() == 1);
    CHECK(pair.at(0)->name == "\xF0\x9F\x8E\xB5 Bass");
}
