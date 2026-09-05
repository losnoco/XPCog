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
#include "xpcog/core/library/PlaylistEntry.hpp"

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

    /// Apple epoch seconds (2001-01-01), exactly as Core Data wrote them.
    /// Zero where Cog recorded no date -- both columns are optional, and a row
    /// created but never played carries neither -- which stays distinguishable
    /// from 1 January 2001 on purpose: an import writes only a date it was
    /// actually given.
    ///
    /// Left in Cog's units, with the conversion in the two accessors below, so
    /// that the thirty-one year offset lives in one place with a test on it
    /// rather than at each call site that wants a timestamp.
    double firstSeen  = 0.0;
    double lastPlayed = 0.0;

    /// The same two dates in Unix seconds, and 0 for a date Cog never recorded.
    [[nodiscard]] std::int64_t firstSeenUnix() const noexcept;
    [[nodiscard]] std::int64_t lastPlayedUnix() const noexcept;
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

/// What turning a `CogLibrary` into playlist entries produced.
///
/// The counts are here for the same reason `CogLibrary`'s prunes are: "your
/// Cog playlist had 900 tracks and this imported 847" is a question somebody
/// will ask, and the useful answer is the shape of the difference rather than
/// an apology.
struct CogPlaylistImport {
    /// In Cog's order.
    std::vector<PlaylistEntry> entries;

    /// How many carry stream properties taken from the store. A scan that
    /// reaches the file overwrites them; this counts what would still say
    /// something if it does not.
    std::size_t withCachedProperties = 0;

    /// How many carry ReplayGain from the store. Worth taking rather than
    /// rescanning -- it is exactly what a rescan would find again, and computing
    /// it is the expensive half.
    std::size_t withReplayGain = 0;

    /// Entries whose URL is a macOS file reference (`file:///.file/id=`).
    /// Included rather than dropped, and marked as errors, because a silently
    /// shorter playlist is worse than a visible row that says it cannot be
    /// found. Always 0 on a store written by a current Cog, which normalises
    /// them on load.
    std::size_t fileReferences = 0;

    /// Where Cog's playback had got to, as an index into `entries`. The entry
    /// also carries `currentPosition`.
    std::optional<std::size_t> currentIndex;
};

/// Turns a library read by `readCogLibrary()` into playlist entries.
///
/// **Tags are deliberately absent.** Nothing here fills in artist, album or
/// title, because the store's copy of those is a cache of unknown age and this
/// reader does not touch the metadata blob at all -- see the note at the top of
/// this header. The entries come out with `metadataLoaded` false, which is the
/// caller's cue to scan them; a scan reads the files, which is both more current
/// and work that has to happen anyway.
///
/// What *is* carried is everything the files cannot say for themselves: the
/// order, the queue, the shuffle order, where the last session had got to, and
/// ReplayGain.
[[nodiscard]] CogPlaylistImport cogLibraryToPlaylist(const CogLibrary& library);

/// Puts back what the store knew and a scan cannot find out.
///
/// `fromStore` is what `cogLibraryToPlaylist()` produced; `scanned` is what the
/// scanner returned for the same URLs. Returns how many entries were matched.
///
/// **Matched by URL rather than by position.** A scan does not answer
/// one-for-one: it drops what it cannot open and expands what turns out to be a
/// container, so the two sequences are not the same length and an index would
/// pair a row with somebody else's ReplayGain -- a fault that is silent, since
/// the wrong gain is still a plausible gain.
///
/// **The scan wins wherever it established anything**, because it read the file
/// and the store is a cache of unknown age. What is put back is only what a file
/// cannot say about itself:
///
///   * The queue position, the shuffle order, and where the last session had got
///     to. None of these is a property of the audio.
///   * ReplayGain, **only where the scan found none**. A file carrying its own
///     tags is more current than Cog's copy; a file carrying none gets Cog's
///     analysis rather than nothing, which is the whole reason it is worth
///     taking from the store -- recomputing it is the expensive half of a scan.
///   * The cached stream properties, likewise only to fill a gap, so a row still
///     says something about a file on a drive that is not plugged in.
std::size_t mergeCogStoreData(std::span<const PlaylistEntry> fromStore,
                              std::span<PlaylistEntry>       scanned);

/// One entry that matched a store row, with everything the row had to say.
///
/// A copy rather than a pointer into the CogPlayCounts it came from, and only
/// of the numbers: the strings are already on the entry, and the caller is
/// writing these into a library row keyed on that entry.
struct CogPlayCountMatch {
    /// Index into the span that was passed to applyCogPlayCounts().
    std::size_t entry = 0;

    std::int64_t count = 0;

    /// Unix seconds, zero where Cog recorded no date. See CogPlayCount.
    std::int64_t firstSeen  = 0;
    std::int64_t lastPlayed = 0;

    /// Zero where Cog recorded no rating, which is how Cog itself says "unrated".
    double rating = 0.0;
};

/// What matching Cog's play counts onto scanned entries produced.
struct CogPlayCountReport {
    /// The entries that received a count, in the order they were passed.
    ///
    /// The dates and the rating ride along here rather than on the entry
    /// because a PlaylistEntry has nowhere to put them: they belong to the
    /// library's play-count row, and the caller is what has a library.
    std::vector<CogPlayCountMatch> matches;

    /// How many entries received a count -- `matches.size()`, kept as a number
    /// because that is all most callers want from this.
    std::size_t matched = 0;

    /// Entries that did not. Counted per *entry* rather than per store row, so
    /// this and `matched` sum to the number of entries passed in. How many of
    /// Cog's rows went unused is the other question, and the caller can ask it
    /// by comparing `matched` against `CogPlayCounts::size()` -- a store row
    /// that matches nothing usually means the file was renamed or is not in
    /// this playlist, neither of which is an error.
    std::size_t unmatched = 0;
};

/// Copies Cog's play counts onto entries that have already been scanned.
///
/// **After a scan, not before, and that order is forced** rather than chosen:
/// the title and artist a row is matched on are the ones the scanner read from
/// the file, because this import does not carry Cog's cached copies. Calling
/// this on unscanned entries matches on empty strings and is worse than not
/// calling it -- an empty field does not constrain the match, so every row would
/// collide with every other.
///
/// Cog's own key is (filename, title, artist, album), where the filename keeps
/// its fragment so cue sheet tracks stay apart -- which is exactly what
/// `PlaylistEntry::filename()` returns.
///
/// The count lands on the entry; the dates and the rating come back in the
/// report, for a caller with a library to merge them into.
CogPlayCountReport applyCogPlayCounts(const CogPlayCounts&     counts,
                                      std::span<PlaylistEntry> entries);

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
