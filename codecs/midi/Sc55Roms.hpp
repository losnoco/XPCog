// Finding a Roland SC-55 ROM set, and knowing it is the right one.
//
// Port of the identification half of Cog Preferences/MIDIConfig.mm. The rest of
// that file installs the ROMs into an application-support folder so its player
// can read them back by a fixed path; nothing here copies anything. The setting
// names wherever the listener already keeps them and this reads them there, for
// the reason docs/MIDI.md gives: these are 3.6 MB of commercial firmware, and a
// player that quietly makes its own second copy of them is a player that has
// decided something on the listener's behalf.
//
// ---------------------------------------------------------------------------
// Identified by hash, not by name
// ---------------------------------------------------------------------------
// nuked-sc55 asks for `rom1.bin`, `waverom2.bin` and so on (mcu.cpp's romset
// table). A dump in the wild is named after Roland's part numbers --
// `r15199858_main_mcu.bin` -- and no two dumps agree on much else, so the names
// on disk cannot be trusted to mean anything.
//
// Cog settles it with SHA-256 and so does this, using the same table. That buys
// more than a rename: it is the difference between "a file of the right length"
// and "the ROM this emulator was written against", and a wrong dump does not
// fail loudly, it boots into a machine that sounds subtly wrong.
//
// The set must also be complete and of one model. Five files of an SC-55mkII
// and one stray mk1 waverom is not a machine.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::codecs {

/// One identified ROM, under the name nuked-sc55 will ask for.
struct Sc55Rom {
    std::string            name;
    std::vector<std::byte> data;
};

/// A complete set for one model.
struct Sc55RomSet {
    /// Cog's device label -- "SC-55mk2" or "SC-55mk1".
    std::string          device;
    std::vector<Sc55Rom> roms;

    [[nodiscard]] const std::vector<std::byte>* find(std::string_view name) const;
};

/// Reads a ROM set from `path`, which may be either a folder of ROM files or
/// the archive they were distributed in.
///
/// The archive case is why this exists rather than a directory scan: a dumped
/// set arrives as a zip and asking someone to unpack it, work out which of five
/// part numbers is `waverom1.bin`, and rename them is asking them to do by hand
/// the one thing a hash table does perfectly. Cog accepts zip, rar and 7z here;
/// libarchive covers those and more.
///
/// Empty when the path holds no complete set, which is not an error worth
/// distinguishing from an empty folder -- both mean the same thing to the
/// listener, which is that this synthesiser cannot start.
[[nodiscard]] std::optional<Sc55RomSet> loadSc55Roms(const std::filesystem::path& path);

}  // namespace xpcog::codecs
