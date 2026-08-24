// Reading a Cog library.
//
// The schema, where it lives, and how each column was established are in
// docs/COGIMPORT.md. This is the reader; that page is why it looks like this.
//
// Portable on purpose. Reading the store is SQLite and nothing else, so it is
// here in core and tested on every platform, while *finding* a Cog installation
// is macOS-only -- Cog runs nowhere else, security-scoped bookmarks resolve
// nowhere else, and a file reference URL resolves by inode through Foundation.
// Those are all about reaching the files; none of them is about reading the
// database, and conflating the two would leave this untestable on four of the
// five CI jobs.
//
// **Metadata is deliberately not read.** Every entry carries a `ZMETADATABLOB`
// holding an NSKeyedArchiver graph of the tags Cog last saw, and this ignores
// it: XPCog's scanner reads tags from the files, which is more current than a
// cache of unknown age and is work that has to happen anyway. What comes out of
// here is where the music is and what order it was in -- the two things the
// files cannot say for themselves.

#pragma once

#include "xpcog/core/Url.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

class Settings;

/// One playlist entry, already pruned and in playlist order.
struct CogEntry {
    Url url;

    /// Cog's own ordering, kept rather than implied by position so that a
    /// caller can tell a gap from a reordering. Dense from 0 on every store
    /// seen, but that is an observation and not a guarantee.
    std::int64_t index = 0;

    /// Cached stream properties. Carried because they cost nothing and a rescan
    /// may not reach the file -- a network mount that is not mounted, a drive
    /// that is not plugged in. A scan that succeeds should overwrite them.
    std::int64_t totalFrames = 0;
    double       sampleRate  = 0.0;
    std::int64_t bitrate     = 0;
    std::int64_t channels    = 0;
    std::string  codec;

    /// ReplayGain, in dB and linear peak, as Cog stored it. Worth taking from
    /// the store rather than rescanning: it is exactly what a rescan would find
    /// again, and computing it is the expensive part.
    double replayGainTrackGain = 0.0;
    double replayGainTrackPeak = 0.0;
    double replayGainAlbumGain = 0.0;
    double replayGainAlbumPeak = 0.0;

    /// Where the last session had got to. `current` marks at most one entry.
    bool   current         = false;
    double currentPosition = 0.0;

    /// The play queue. `queuePosition` is -1 when not queued, which is Cog's
    /// own sentinel rather than one invented here.
    bool         queued        = false;
    std::int64_t queuePosition = -1;

    std::int64_t shuffleIndex = 0;

    /// A macOS file reference URL -- `file:///.file/id=…`, naming a file by
    /// inode. `url` still holds it verbatim, because resolving one needs
    /// Foundation and this layer has none. A caller on macOS should resolve
    /// before using it; a caller anywhere else should treat it as unreachable
    /// rather than as a path.
    bool fileReference = false;
};

/// One row of ZPLAYCOUNT.
///
/// Keyed by strings rather than by a relationship, which is a fact about Cog's
/// model and the reason matching one back to a track is a decision rather than
/// a join. See CogPlayCounts.
struct CogPlayCount {
    /// The URL's last path component, **including any fragment** --
    /// "Album.cue#01". Never empty on any row observed.
    std::string filename;
    std::string title;
    std::string artist;
    std::string album;

    std::int64_t count  = 0;
    double       rating = 0.0;

    /// Apple epoch seconds (2001-01-01), left unconverted: nothing here reads
    /// them, and a conversion nobody checks is a conversion that is wrong.
    double firstSeen  = 0.0;
    double lastPlayed = 0.0;
};

/// Play counts, indexed for the lookup Cog's own keying forces.
///
/// **The match is on the full tuple, and an empty field does not constrain it.**
/// That second half is not a softening of the first, it is what makes it usable:
/// on the store this was built against, 78 of 84 rows have no artist and no
/// album, so requiring all four to be equal would import 6 of them. A field Cog
/// never recorded cannot disagree with anything, so it is skipped; every field
/// it did record must match exactly.
///
/// In practice that means filename and title, which were populated on every row
/// seen and which together were unique across all 84 -- the fragment in the
/// filename is what keeps cue sheet tracks apart. Artist and album narrow it
/// further when they are there.
///
/// Matching happens *after* a rescan rather than at import, and that is forced
/// by not reading the metadata blob: the title and artist to match on are the
/// ones the scanner reads from the file, not ones carried out of the store.
class CogPlayCounts {
public:
    CogPlayCounts() = default;
    explicit CogPlayCounts(std::vector<CogPlayCount> counts);

    /// The count for a track, or nullptr when nothing matches.
    ///
    /// `filename` is compared as Cog stored it: the last path component with the
    /// fragment still attached. Url::fileName() does not include one, so a
    /// caller with a cue track has to append it -- which is why this takes the
    /// string rather than a Url, so the joining happens once at the call site
    /// that knows the shape rather than being guessed at here.
    [[nodiscard]] const CogPlayCount* find(std::string_view filename,
                                           std::string_view title,
                                           std::string_view artist,
                                           std::string_view album) const;

    [[nodiscard]] std::span<const CogPlayCount> all() const noexcept { return counts_; }
    [[nodiscard]] bool        empty() const noexcept { return counts_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return counts_.size(); }

private:
    std::vector<CogPlayCount> counts_;
};

/// What a read produced, including what it threw away.
struct CogLibrary {
    /// In playlist order, with everything Cog itself would prune removed.
    std::vector<CogEntry> entries;

    CogPlayCounts playCounts;

    /// The paths Cog held security-scoped bookmarks for, from ZSANDBOXTOKEN.
    /// The bookmarks themselves are not read: they are opaque, only macOS can
    /// resolve them, and the path beside each one is plain text. Useful as the
    /// list of places to ask the user to grant access to, which is the only
    /// thing a non-sandboxed player needs from them.
    std::vector<std::string> sandboxPaths;

    /// Counted rather than silently dropped, because "your playlist had 900
    /// tracks and this imported 847" is a question someone will ask, and the
    /// honest answer is the shape of what was skipped.
    std::size_t prunedDeleted     = 0;
    std::size_t prunedEmptyUrl    = 0;
    std::size_t prunedUnparseable = 0;

    /// How many entries hold a file reference URL. Zero on a store written by a
    /// current Cog, which normalises them on load.
    std::size_t fileReferences = 0;
};

/// True for a serialised macOS file reference URL. Syntactic; touches nothing.
[[nodiscard]] bool isCogFileReferenceUrl(std::string_view urlString);

/// What a settings import did, reported rather than assumed.
struct CogSettingsReport {
    /// Keys XPCog knows, copied across.
    std::size_t applied = 0;

    /// Keys that were in the file and mean nothing here. Cog-only settings
    /// (`pitch`, `tempo`, `miniPlusMode`), and AppKit's own window and toolbar
    /// state, which is most of a real file by count.
    std::size_t ignored = 0;

    /// Present, known, and of a shape that would not convert -- an array where
    /// a number was declared. Counted separately from `ignored` because it means
    /// the two programs disagree about a key they share, which is worth knowing
    /// and is not the same as a key one of them has never heard of.
    std::size_t mismatched = 0;

    /// The keys that were applied, for a caller that wants to say what changed.
    std::vector<std::string> appliedKeys;
};

/// Copies a Cog defaults plist into `settings`.
///
/// `plistXml` is the file as XML text. Getting it there is
/// `platform::propertyListToXml()`, because a real one is `bplist00` and core's
/// plist reader is XML-only -- see that header for why the conversion lives
/// where it does.
///
/// **The filter is free, and that is the whole trick.** A key is imported when
/// `Settings::all()` has one by the same name, and ignored otherwise. There is
/// no table of translations here and there should never be one: `settings.def`
/// kept Cog's spellings deliberately, so the two programs already agree about
/// what `eq1kHz` and `volumeScaling` are called and what range they hold. A
/// hand-written mapping would be a second copy of that agreement, free to drift
/// from it.
///
/// **A key that is absent is not touched.** `NSUserDefaults` persists only what
/// differs from what the app registered at launch, so a real file holds a
/// handful of keys and the absence of `eq1kHz` means "Cog's default", which is
/// XPCog's default too. Writing defaults over the settings for keys the file
/// never mentioned would overwrite the user's XPCog configuration with Cog's,
/// which is the opposite of an import.
void importCogSettings(std::string_view plistXml, Settings& settings,
                       CogSettingsReport* report = nullptr);

/// Reads a Cog Core Data store.
///
/// Takes a path rather than finding one, so that it can be pointed at a copy --
/// which is what a caller should do. The store is WAL-mode and Cog may be
/// running, so a copy has to bring `-wal` and `-shm` with it or recent entries
/// are simply absent.
///
/// nullopt when the file will not open or is not a Cog store. An empty library
/// is a valid answer and a different one: a store with no entries in it.
[[nodiscard]] std::optional<CogLibrary> readCogLibrary(const std::filesystem::path& store);

}  // namespace xpcog
