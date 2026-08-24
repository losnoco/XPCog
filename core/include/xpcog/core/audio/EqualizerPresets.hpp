// The equaliser's preset library. Port of Cog's EqualizerWindowController.m,
// minus the window.
//
// Cog keeps the presets in a JSON file it ships -- `Cog.q1.json`, a "Cog EQ
// library file v1.0" -- rather than in the code, which is what makes the set
// editable without a rebuild and shareable between installs. XPCog ships that
// same file verbatim, so a library a Cog user has curated drops in unchanged.
//
// The one thing worth knowing before reading any of this: **a preset does not
// store the 31 bands the equaliser actually has.** It stores ten, at the octave
// centres a ten-band equaliser uses (32 Hz to 16 kHz), and Cog interpolates
// those ten points onto Apple's 31. That is not a shortcut -- it is what lets a
// preset written for any equaliser mean something here, and it is why
// `interpolateEqualizerPreset()` is the only place a preset ever becomes a
// curve.
//
// The interpolation is Cog's, term for term, including the part that has to
// invent data: three of the 31 centres (20, 25 and 31.5 Hz) sit *below* the
// lowest point a preset stores, and one (20 kHz) sits above the highest. Cog
// extrapolates by continuing the last step in both gain and frequency, scaled
// by 1.05, for four more synthetic points, then interpolates within those. The
// comment in Cog's source records that lpc and quadratic extrapolation were both
// tried first and were both worse, which is worth preserving here because the
// formula looks arbitrary until you know it is the third attempt.
//
// It is also, for the band table XPCog actually has, worth knowing that the 1.05
// does nothing. The synthetic points are generated *along the line* through the
// two outermost stored points, so interpolating between the outermost real point
// and a synthetic one gives the same answer whatever the step is scaled by --
// the scale cancels. All four extrapolated centres land inside the first
// synthetic interval, so every one of them is simply the outermost segment
// continued as a straight line, and `1.05` could be any positive number without
// changing a single gain. The code keeps Cog's form anyway: the cancellation
// stops holding the moment a centre falls past the first synthetic point, and a
// band table is a thing that could change. test_eqpresets.cpp checks the closed
// form rather than the loop, which is what makes that claim checkable rather
// than merely asserted.
//
// Two deliberate differences from Cog, both narrow:
//
//   * The document's members are looked up by name rather than by position.
//     Cog requires the top-level object to hold exactly two members, "type"
//     then "presets", in that order, and rejects the file otherwise. Matching
//     that would only ever reject a file Cog's own format allows, so this
//     accepts any object carrying both -- strictly more permissive, and
//     incapable of rejecting something Cog reads.
//   * `altGenres` actually reads the alternate genres. Cog's loop indexes its
//     value array with the *preset's* index instead of the entry's
//     (EqualizerWindowController.m:200), so an alias list reads the wrong
//     element or runs off the end. The bug is invisible in the shipped library
//     because no preset in it declares aliases, and it would stop being
//     invisible the moment a user's library did.
//
// Arithmetic is double here where Cog's is float. The difference lands around
// the seventh decimal of a dB -- two orders of magnitude below the 0.1 dB the
// interface can even display -- and double is what the settings store and the
// filter already use, so converting to float and back would be the only reason
// to introduce a rounding step.

#pragma once

#include "xpcog/core/audio/Equalizer.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

class Settings;

/// One entry from a preset library.
struct EqualizerPreset {
    /// The number of points a preset stores, which is not Equalizer::kBands.
    static constexpr int kPoints = 10;

    /// The preset's name, which is also what a genre is matched against.
    std::string name;

    /// Gain in dB at each of frequencies(), low to high.
    std::array<double, kPoints> gainsDb{};

    /// Gain in dB applied ahead of the bands.
    double preampDb = 0.0;

    /// Extra genre names that select this preset, from the optional
    /// `altGenres` member. Empty for every preset in the shipped library.
    std::vector<std::string> altGenres;
};

/// A parsed "Cog EQ library file v1.0".
class EqualizerPresetLibrary {
public:
    /// The ten frequencies in Hz a preset stores, low to high.
    [[nodiscard]] static std::span<const double> frequencies() noexcept;

    /// The `type` string a document must carry to be read at all.
    [[nodiscard]] static std::string_view documentType() noexcept;

    /// Parses one library document.
    ///
    /// A document that is not a library -- unparseable, the wrong type string,
    /// no presets array -- yields an empty library rather than an error,
    /// because every caller's answer to a broken library is the same one it
    /// gives to a missing one: carry on with the curve the user already has.
    /// Individual presets missing any of the twelve required members are
    /// dropped and the rest are kept, which is Cog's behaviour too.
    [[nodiscard]] static EqualizerPresetLibrary parse(std::string_view document);

    /// Reads the library XPCog ships beside itself. Empty when it is missing,
    /// which is a headless build or a broken install rather than anything a
    /// user does.
    [[nodiscard]] static EqualizerPresetLibrary loadShipped();

    [[nodiscard]] std::span<const EqualizerPreset> presets() const noexcept {
        return presets_;
    }
    [[nodiscard]] bool        empty() const noexcept { return presets_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return presets_.size(); }

    /// The preset at `index`, or nullptr when the index names none.
    ///
    /// Takes a signed index on purpose: the two values that mean "no preset" are
    /// Cog's -1, the default, and size(), which is the "Custom" row at the end
    /// of the selector. Both arrive here from settings, and both have to answer
    /// nullptr rather than wrap around.
    [[nodiscard]] const EqualizerPreset* at(int index) const noexcept;

    /// The index of the preset named exactly `name`, or -1. Aliases count.
    [[nodiscard]] int indexOf(std::string_view name) const noexcept;

    /// The preset index Cog would pick for a track's genre, or -1 when the
    /// library has nothing to offer -- which, since the fallback is the preset
    /// named "Flat", means a library without one.
    ///
    /// Three steps, in Cog's order: an exact name match including aliases;
    /// failing that, the *longest* preset name that appears anywhere inside the
    /// genre, compared case-insensitively, so "Progressive Rock" finds "Rock"
    /// and a hypothetical "Punk Rock" preset would beat it; failing that,
    /// "Flat".
    ///
    /// An empty genre skips straight to Flat. That is Cog's behaviour and it is
    /// worth stating plainly, because it means turning genre tracking on
    /// flattens the equaliser for every untagged track rather than leaving it
    /// alone.
    [[nodiscard]] int matchGenre(std::string_view genre) const noexcept;

private:
    std::vector<EqualizerPreset> presets_;
};

/// The shipped library, read once on first use.
///
/// A function-local static rather than an object someone owns: the preset list
/// is immutable, three unrelated places need it (the selector, genre tracking,
/// and whatever asks what "Flat" means), and the file is 6 KB. Cog caches it in
/// a file-scope global for the same reason.
[[nodiscard]] const EqualizerPresetLibrary& shippedEqualizerPresets();

/// `preset`'s ten points interpolated onto the equaliser's 31 centres.
[[nodiscard]] std::array<double, Equalizer::kBands> interpolateEqualizerPreset(
    const EqualizerPreset& preset);

/// Writes `preset` into `settings`: `eqPreamp` and all 31 band keys.
///
/// The settings are the record, not the filter. Applying a preset means writing
/// the curve it produces, exactly as dragging 32 sliders would -- so nothing
/// downstream has to know a preset was involved, and a preset and a hand-made
/// curve are the same kind of thing by the time the engine reads them.
void applyEqualizerPreset(Settings& settings, const EqualizerPreset& preset);

}  // namespace xpcog
