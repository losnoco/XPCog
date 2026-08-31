// What the remote control is allowed to ask the player to do.
//
// The server lives in core and the player does not: the transport is
// PlaybackController in app/, above the toolkit line, and the playlist, the undo
// stack and the library are all reached through the window. So core names an
// interface and the layer that has those things implements it -- app/ over
// PlaybackController and AppCommands, xpcog-cli over an AudioEngine and a
// SerialExecutor.
//
// Two rules hold this shape, and both are load-bearing:
//
// **No JSON here.** Methods speak std:: types and TrackId. nlohmann is linked
// PRIVATE to xpcog-core deliberately -- LastFmClient.hpp speaks std::string for
// the same reason -- and a public header naming it would end that. Serialisation
// happens in core/src/remote, on the HTTP thread, from the plain structs these
// return.
//
// **Every method runs on the interface thread, one at a time.** CallGate puts
// them there and waits. So an implementation may touch Playlist, UndoStack,
// Library and Settings unlocked, exactly as a menu handler does, and must not
// assume anything else about which thread it is on.
//
// --- Why Outcome and not bool ----------------------------------------------
//
// PlaybackController guards a start in flight with starting_/stopping_ and
// silently declines commands while one is running. An API that answered 200 for
// a decline would be lying in the first place it matters, so a command answers
// with a reason and the router turns Busy into 409 rather than 200.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::remote {

/// Why a command did not happen.
enum class Outcome {
    Ok,
    /// A start or stop is in flight and the player would decline this. 409.
    Busy,
    /// No such track, key or preset. 404.
    NotFound,
    /// Understood and refused -- a value out of range, a mode that is not one of
    /// the names. 400.
    Rejected,
    /// A setting that is session state rather than a preference. 403.
    ReadOnly,
    /// This host cannot do it. xpcog-cli answers this for the things only the
    /// window can do, and says which in its --help. 501.
    Unsupported,
};

/// What a list row needs, and nothing more.
///
/// A summary rather than the entry because a playlist can be tens of thousands
/// of rows and the whole MetadataMap of each is not what a list is for. The
/// detail view asks for one track.
struct TrackSummary {
    TrackId       id = kInvalidTrackId;
    std::string   url;
    std::string   title;
    std::string   artist;
    std::string   album;
    double        duration  = 0.0;
    std::int32_t  track     = 0;
    std::int32_t  disc      = 0;
    std::int64_t  playCount = 0;
    bool          queued    = false;
    bool          stopAfter = false;
    bool          error     = false;
    bool          hasArtwork = false;
};

/// One track in full: the summary, the decoded properties, and every tag.
struct TrackDetail {
    TrackSummary summary;
    std::string  errorMessage;
    std::string  lyrics;
    std::string  genre;
    std::string  composer;
    std::string  date;
    std::string  comment;
    std::string  albumArtist;

    double       sampleRate  = 0.0;
    int          channels    = 0;
    int          bitsPerSample = 0;
    int          bitrateKbps = 0;
    bool         seekable    = false;
    bool         lossless    = false;
    bool         cuesheet    = false;

    /// Every tag the file carried, including the ones no field above promotes.
    std::vector<std::pair<std::string, std::string>> metadata;
};

/// One page of the playlist, and where the playing track is relative to it.
struct TrackPage {
    std::vector<TrackSummary> items;

    /// How many matched the query, which is what a pager needs.
    std::size_t total = 0;

    /// The row the playing track sits at in this filter and this sort.
    ///
    /// Nothing when nothing is playing, or when the query filters it out. It is
    /// an index into the whole filtered result rather than into `items`, so it
    /// answers the question `items` cannot: a client wanting to scroll to what is
    /// playing has to know which page to ask for, and a per-item "is this it"
    /// flag is false on every row of a page that does not contain it -- which
    /// reads exactly like nothing playing at all.
    std::optional<std::size_t> currentRow;
};

/// What GET /status answers, and the only thing the interface pushes out rather
/// than being asked for.
struct Status {
    bool    playing = false;
    bool    paused  = false;
    TrackId currentTrack = kInvalidTrackId;
    double  position = 0.0;
    double  duration = 0.0;
    double  volume   = 0.0;

    std::string repeat  = "none";   ///< none | one | album | all
    std::string shuffle = "off";    ///< off | albums | all
    bool        stopAfterCurrent = false;

    std::size_t playlistSize = 0;

    /// Bumped on every change to the playlist, and reset per launch along with
    /// sessionId.
    ///
    /// TrackIds are session-scoped: nothing persists them, and the same number
    /// means a different track after a restart. A client that cached ids and
    /// acted on them later would delete the wrong track. These two are how it
    /// finds out -- when either moves, what it cached is stale.
    std::uint64_t playlistRevision = 0;
    std::string   sessionId;
};

/// One setting, as the API reports it.
struct SettingInfo {
    std::string key;
    std::string type;          ///< bool | int | double | std::string
    std::string value;
    std::string defaultValue;
    bool        writable = true;
    /// immediately | nextTrack | nextDeviceOpen | nextScan | nextLaunch
    std::string appliesFrom;
};

/// The answer to a settings write: whether it took, and when it will be heard.
struct SettingWrite {
    Outcome     outcome = Outcome::Ok;
    std::string appliesFrom;
    std::string message;
};

/// The 31 bands, the preamp and the switch.
struct EqualizerState {
    bool                enabled = false;
    double              preamp  = 0.0;
    /// Frequency and gain, in the fixed band order.
    std::vector<std::pair<double, double>> bands;
    bool                trackGenre = false;

    /// The preset this curve is, or empty when it is not one of them.
    ///
    /// Reported because the curve alone does not say it, and a client that has
    /// just applied "Rock" wants to see that it took rather than compare 31
    /// numbers against a list.
    std::string         preset;
};

/// A scan, which is the one thing here that can take minutes.
struct JobStatus {
    std::string id;
    std::string state;   ///< queued | running | done | failed
    int         done  = 0;
    int         total = 0;
    std::size_t added = 0;
    std::string error;
};

/// Everything the remote control may ask the player to do.
///
/// Nothing here reveals a file in a file manager, moves one to the trash, or
/// writes a playlist to a path the request names. Adding tracks already implies
/// reading anything the player can decode, which is inherent and is what the
/// token is for -- but filesystem *write* and *delete* driven by a network peer
/// are a different order of consequence, and they stay in the window.
class IPlayerControl {
public:
    virtual ~IPlayerControl() = default;

    IPlayerControl()                                 = default;
    IPlayerControl(const IPlayerControl&)            = delete;
    IPlayerControl& operator=(const IPlayerControl&) = delete;

    // --- transport ---------------------------------------------------------

    [[nodiscard]] virtual Status status() = 0;

    /// Plays `id`, or resumes what is loaded when it is nullopt.
    virtual Outcome play(std::optional<TrackId> id) = 0;
    virtual Outcome pause()     = 0;
    virtual Outcome playPause() = 0;
    virtual Outcome stop()      = 0;
    virtual Outcome next()      = 0;
    virtual Outcome previous()  = 0;
    virtual Outcome seek(double seconds) = 0;
    virtual Outcome setVolume(double linear) = 0;

    /// Any of the three, each optional. Names rather than numbers, because the
    /// numbers are Cog's enum values and the API should not be a way to learn
    /// them.
    virtual Outcome setOrder(std::optional<std::string> repeat,
                             std::optional<std::string> shuffle,
                             std::optional<bool>        stopAfterCurrent) = 0;

    // --- playlist ----------------------------------------------------------

    /// One page of the playlist, filtered by `query` when it is not empty.
    [[nodiscard]] virtual TrackPage tracks(std::size_t offset, std::size_t limit,
                                           std::string_view query) = 0;

    [[nodiscard]] virtual std::optional<TrackDetail> track(TrackId id) = 0;

    /// Starts adding `urls` at `at`, or at the end. Answers a job id, because a
    /// directory of ten thousand files is not something to hold a request open
    /// for. Empty when this host cannot scan.
    [[nodiscard]] virtual std::string addUrls(std::vector<std::string> urls,
                                              std::optional<std::size_t> at) = 0;

    virtual Outcome removeTracks(const std::vector<TrackId>& ids) = 0;

    /// Moves `ids` before `anchor`, or to the end for kInvalidTrackId.
    virtual Outcome moveTracks(const std::vector<TrackId>& ids, TrackId anchor) = 0;

    virtual Outcome randomize()     = 0;
    virtual Outcome clearPlaylist() = 0;

    virtual Outcome setQueued(const std::vector<TrackId>& ids, bool queued) = 0;
    virtual Outcome clearQueue() = 0;
    virtual Outcome setStopAfter(const std::vector<TrackId>& ids, bool stopAfter) = 0;
    virtual Outcome resetPlayCount(const std::vector<TrackId>& ids) = 0;
    virtual Outcome setRating(const std::vector<TrackId>& ids,
                              std::optional<double> rating) = 0;

    virtual Outcome undo() = 0;
    virtual Outcome redo() = 0;

    // --- settings and DSP --------------------------------------------------

    [[nodiscard]] virtual std::vector<SettingInfo> settings() = 0;
    [[nodiscard]] virtual std::optional<SettingInfo> setting(std::string_view key) = 0;
    virtual SettingWrite setSetting(std::string_view key, std::string_view value) = 0;

    [[nodiscard]] virtual EqualizerState equalizer() = 0;
    virtual Outcome setEqualizer(std::optional<bool> enabled,
                                 std::optional<double> preamp,
                                 const std::vector<std::pair<double, double>>& bands) = 0;
    [[nodiscard]] virtual std::vector<std::string> equalizerPresets() = 0;
    virtual Outcome applyEqualizerPreset(std::string_view name) = 0;

    // --- artwork and jobs --------------------------------------------------

    /// The cover as the file carried it -- JPEG or PNG bytes, not a decoded
    /// image. A shared_ptr because Library::sharedArtwork() already hands one
    /// back, and copying a five-megabyte cover while the interface thread is
    /// held would block every other request behind it.
    [[nodiscard]] virtual std::shared_ptr<const std::vector<std::byte>> artwork(
        TrackId id) = 0;

    [[nodiscard]] virtual std::optional<JobStatus> job(std::string_view id) = 0;
};

}  // namespace xpcog::remote
