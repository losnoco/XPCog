#include "midi/Sc55Roms.hpp"

#include "archive/ArchiveReader.hpp"

#include "xpcog/core/Sha256.hpp"

#include <archive_entry.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>

namespace xpcog::codecs {
namespace {

/// Cog's table, verbatim (Preferences/MIDIConfig.mm, +nukedRomsets).
///
/// Hashes only. Matching on file size as well would make a folder scan cheaper,
/// and is left out because half the table's sizes are not something this
/// machine can check -- there is an SC-55mkII dump here and no mk1 -- and a
/// guessed size does not fail loudly, it silently refuses a valid set. The
/// bound below does the job the sizes would have done.
///
/// Cog carries a third set for the JV-880, commented out, and seven more device
/// names with no hashes at all. Those are not omissions to fix: nuked-sc55
/// autodetects by probing for filenames, and a set it has no hashes for is a
/// set nobody can identify.
struct KnownRom {
    std::string_view sha256;
    std::string_view name;
    std::string_view device;
};

constexpr std::string_view kSc55mk2 = "SC-55mk2";
constexpr std::string_view kSc55mk1 = "SC-55mk1";

constexpr KnownRom kKnownRoms[] = {
    {"8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
     "rom1.bin", kSc55mk2},
    {"a4c9fd821059054c7e7681d61f49ce6f42ed2fe407a7ec1ba0dfdc9722582ce0",
     "rom2.bin", kSc55mk2},
    {"b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
     "rom_sm.bin", kSc55mk2},
    {"c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
     "waverom1.bin", kSc55mk2},
    {"5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
     "waverom2.bin", kSc55mk2},

    {"7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544",
     "sc55_rom1.bin", kSc55mk1},
    {"effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1",
     "sc55_rom2.bin", kSc55mk1},
    {"5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
     "sc55_waverom1.bin", kSc55mk1},
    {"c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
     "sc55_waverom2.bin", kSc55mk1},
    {"334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2",
     "sc55_waverom3.bin", kSc55mk1},
};

/// How many files each model's set is. Cog keeps this in +nukedDevices, and it
/// is a check rather than a formality: a partial set autodetects as some other
/// model and boots into a machine nobody asked for.
[[nodiscard]] std::size_t expectedCount(std::string_view device) {
    return static_cast<std::size_t>(
        std::count_if(std::begin(kKnownRoms), std::end(kKnownRoms),
                      [device](const KnownRom& rom) { return rom.device == device; }));
}

/// The largest file worth reading at all. The biggest ROM in the table is
/// 2 MiB, so this is generous by a factor of four and still keeps the scan off
/// everything a ROM folder might sit beside -- the SoundFont this was developed
/// against is 1.26 GiB, and hashing it to discover it is not a ROM would be the
/// slowest possible way to find that out. Also bounds an archive entry, whose
/// declared size is not trustworthy for every format.
constexpr std::uintmax_t kMaxRomBytes = 8U * 1024U * 1024U;

[[nodiscard]] const KnownRom* identify(std::span<const std::byte> data) {
    const std::string hash = sha256Hex(data);
    for (const KnownRom& rom : kKnownRoms) {
        if (rom.sha256 == hash) {
            return &rom;
        }
    }
    return nullptr;
}

/// Accumulates identified files, refusing anything that would mix two models.
class SetBuilder {
public:
    /// Takes `data` if it is a ROM this table knows. Returns false only when the
    /// file belongs to a *different* model than the ones already accepted, which
    /// is the one case worth stopping on -- everything else unrecognised is
    /// simply not a ROM and is skipped.
    bool offer(std::vector<std::byte> data) {
        const KnownRom* known = identify(data);
        if (known == nullptr) {
            return true;
        }
        if (!device_.empty() && device_ != known->device) {
            return false;
        }
        device_ = known->device;
        // Keyed by name rather than appended, so the same ROM found twice --
        // an archive holding a folder and a copy of its contents -- counts once.
        roms_[std::string{known->name}] = std::move(data);
        return true;
    }

    [[nodiscard]] std::optional<Sc55RomSet> build() const {
        if (device_.empty() || roms_.size() != expectedCount(device_)) {
            return std::nullopt;
        }
        Sc55RomSet set;
        set.device = std::string{device_};
        set.roms.reserve(roms_.size());
        for (const auto& [name, data] : roms_) {
            set.roms.push_back({name, data});
        }
        return set;
    }

private:
    std::string_view                                device_;
    std::map<std::string, std::vector<std::byte>>   roms_;
};

[[nodiscard]] std::optional<std::vector<std::byte>> readWholeFile(
    const std::filesystem::path& path, std::uintmax_t size) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfopen(path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return std::nullopt;
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (got != data.size()) {
        return std::nullopt;
    }
    return data;
}

[[nodiscard]] std::optional<Sc55RomSet> loadFromDirectory(
    const std::filesystem::path& path) {
    SetBuilder      builder;
    std::error_code error;
    // Not recursive, which is Cog's behaviour too: a ROM set is a flat handful
    // of files, and descending would mean walking whatever else the folder
    // happens to contain.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{path, error}) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error) || error) {
            continue;
        }
        const std::uintmax_t size = entry.file_size(error);
        if (error || size == 0 || size > kMaxRomBytes) {
            continue;
        }
        auto data = readWholeFile(entry.path(), size);
        if (!data) {
            continue;
        }
        if (!builder.offer(std::move(*data))) {
            return std::nullopt;
        }
    }
    return builder.build();
}

[[nodiscard]] std::optional<Sc55RomSet> loadFromArchive(
    const std::filesystem::path& path) {
    ArchivePtr handle = openArchiveFile(path);
    if (!handle) {
        return std::nullopt;
    }

    SetBuilder      builder;
    archive_entry* entry = nullptr;
    while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;
        }
        const std::int64_t declared = archive_entry_size(entry);
        if (declared > static_cast<std::int64_t>(kMaxRomBytes)) {
            continue;
        }
        auto data = readEntry(handle.get(), declared);
        if (!data || data->size() > kMaxRomBytes) {
            continue;
        }
        if (!builder.offer(std::move(*data))) {
            return std::nullopt;
        }
    }
    return builder.build();
}

}  // namespace

const std::vector<std::byte>* Sc55RomSet::find(std::string_view name) const {
    const auto it = std::find_if(roms.begin(), roms.end(), [name](const Sc55Rom& rom) {
        return rom.name == name;
    });
    return (it == roms.end()) ? nullptr : &it->data;
}

std::optional<Sc55RomSet> loadSc55Roms(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error) {
        return loadFromDirectory(path);
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    return loadFromArchive(path);
}

}  // namespace xpcog::codecs
