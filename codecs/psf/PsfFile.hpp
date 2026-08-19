// The PSF container, shared by every format HighlyComplete plays.
//
// A PSF file is a header, a deflated program image, and a block of tags. What
// makes the format interesting is the `_lib` tag: a `.minigsf` is a few hundred
// bytes of override naming a `.gsflib` that holds the game's entire program, so
// one library serves a whole soundtrack. In the corpus this was written against,
// 783 `.minigsf` files resolve to 12 `.gsflib` files.
//
// The chain is recursive and ordered, and the order is the part worth stating
// because it is not the obvious one. Given a file that names `_lib` and then
// `_lib2`, psflib loads the library chain depth-first and hands the resulting
// program images back **highest priority first** -- the deepest library, then
// its dependents, then the file you asked for, each overlaying the last. An
// emulator core applies them in that order and ends up with the right memory
// image.
//
// This layer does the container and nothing else: it resolves the chain, hands
// out the program images, and parses the tags. What turns a program image into
// audio is an emulator core -- mGBA for GSF, lazyusf2 for USF, vio2sf for 2SF
// and five more -- and each of those lands separately behind this. Until one
// does, nothing here registers a decoder: a decoder that cannot decode is worse
// than a format the player does not claim.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::codecs {

/// The PSF version byte, which says which emulator the program image is for.
/// Named rather than numbered at the call site, since 0x22 meaning "Nintendo DS"
/// is not something to rediscover.
enum class PsfVersion : std::uint8_t {
    Playstation   = 0x01,  ///< psf, minipsf
    Playstation2  = 0x02,  ///< psf2, minipsf2
    Saturn        = 0x11,  ///< ssf, minissf
    Dreamcast     = 0x12,  ///< dsf, minidsf
    GameBoyAdvance = 0x22, ///< gsf, minigsf
    Nintendo64    = 0x21,  ///< usf, miniusf
    NintendoDS    = 0x24,  ///< 2sf, mini2sf
    Snes          = 0x23,  ///< snsf, minisnsf
    Qsound        = 0x41,  ///< qsf, miniqsf
    NcsfDs        = 0x25,  ///< ncsf, minincsf
};

/// One program image from the chain, in the order psflib handed it over, which
/// is load order and not priority order: `_lib` and its own chain come first,
/// then the file itself, then `_lib2`, `_lib3` and so on. A core applies them
/// in exactly this order and lets each write over what came before, so the
/// main file overrides its `_lib` by arriving later.
///
/// Which of the two sections carries the program depends on the console, and it
/// is not a detail a core can ignore: GSF puts the GBA image in `exe`, while USF
/// leaves `exe` empty entirely and keeps the N64 data in `reserved`. PSF2 is
/// likewise a filesystem in `reserved` rather than an executable.
struct PsfProgram {
    std::vector<std::uint8_t> exe;
    std::vector<std::uint8_t> reserved;
};

/// A loaded PSF chain: what the emulator needs, plus what the playlist shows.
struct PsfFile {
    std::uint8_t            version = 0;
    std::vector<PsfProgram> programs;
    MetadataMap             tags;

    /// From the `length` tag, in seconds, or nullopt when the file does not say.
    /// PSF has no intrinsic duration -- the program would run for ever -- so
    /// this tag is the only thing that makes a track finite.
    std::optional<double> length;
    /// From the `fade` tag, in seconds. Applied after `length`.
    std::optional<double> fade;
    /// From the `volume` tag; 1.0 when absent.
    double volume = 1.0;

    [[nodiscard]] bool empty() const noexcept { return programs.empty(); }
};

/// Parses a PSF time: `[[hours:]minutes:]seconds[.fraction]`, in seconds.
///
/// Exposed because it is the one piece of pure logic here and the one that
/// silently ruins a track when it is wrong -- PSF has no intrinsic duration, so
/// the `length` tag is the only thing that makes a track finite, and a
/// misparsed one truncates or extends every track in a set. Note the fraction
/// is decimal, not a frame count, which is the detail anyone arriving from cue
/// sheets gets wrong.
[[nodiscard]] std::optional<double> parsePsfTime(std::string_view text);

/// Loads `url` and everything its `_lib` tags name, through `registry` so a PSF
/// inside an archive resolves its libraries from the same archive.
///
/// `allowedVersion` of 0 probes rather than enforcing, which is what a metadata
/// read wants; a core passes its own version so a mismatched file is refused
/// rather than fed to the wrong emulator.
///
/// `wantNestedTags` also reports the tags of the libraries the chain walks
/// through, which some cores need: a USF's `_enablecompare` may sit in the
/// `.usflib` rather than in the `.miniusf` that names it, and lazyusf2 renders
/// the wrong thing without it. Off by default because it is only ever the
/// underscore tags a core is after -- `length`, `fade`, `volume` and everything
/// shown in the playlist always come from the outermost file, which is the file
/// the user actually selected. A library that carries its own `length` cannot
/// overrule the track that names it, whatever this is set to.
///
/// Returns nullopt when the file is not a PSF, when a named library is missing,
/// or when the chain is malformed.
[[nodiscard]] std::optional<PsfFile> loadPsf(const Url& url,
                                             const PluginRegistry& registry,
                                             std::uint8_t allowedVersion = 0,
                                             bool wantNestedTags = false);

/// Just the tags, without inflating any program image. What a metadata reader
/// wants, and much cheaper: a `.gsflib` can be megabytes and none of it is
/// needed to answer "what is this track called".
[[nodiscard]] std::optional<PsfFile> readPsfTags(const Url& url,
                                                 const PluginRegistry& registry);

}  // namespace xpcog::codecs
