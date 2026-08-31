#include "AppPlayerControl.hpp"

#include "PlaybackController.hpp"
#include "SettingEffect.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/remote/Token.hpp"

#include <algorithm>
#include <variant>
#include <cmath>
#include <exception>
#include <span>
#include <string>
#include <array>
#include <utility>

namespace xpcog::app {
namespace {

using remote::Outcome;

/// Repeat and shuffle as names rather than as Cog's enum values.
///
/// The numbers are stored, because settings.def keeps Cog's keys and Cog's
/// values so a plist imports verbatim. They are not the API's to expose: a
/// client should not have to learn that shuffle 1 means albums.
constexpr std::array<std::pair<const char*, RepeatMode>, 4> kRepeatNames{{
    {"none", RepeatMode::None},
    {"one", RepeatMode::One},
    {"album", RepeatMode::Album},
    {"all", RepeatMode::All},
}};

constexpr std::array<std::pair<const char*, ShuffleMode>, 3> kShuffleNames{{
    {"off", ShuffleMode::Off},
    {"albums", ShuffleMode::Albums},
    {"all", ShuffleMode::All},
}};

std::string nameOf(RepeatMode mode) {
    for (const auto& [name, value] : kRepeatNames) {
        if (value == mode) {
            return name;
        }
    }
    return "none";
}

std::string nameOf(ShuffleMode mode) {
    for (const auto& [name, value] : kShuffleNames) {
        if (value == mode) {
            return name;
        }
    }
    return "off";
}

std::optional<RepeatMode> repeatFrom(std::string_view name) {
    for (const auto& [text, value] : kRepeatNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<ShuffleMode> shuffleFrom(std::string_view name) {
    for (const auto& [text, value] : kShuffleNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

remote::TrackSummary summarise(const PlaylistEntry& entry) {
    remote::TrackSummary track;
    track.id         = entry.id;
    track.url        = entry.url.toString();
    track.title      = entry.title();
    track.artist     = entry.artist.str();
    track.album      = entry.album.str();
    track.duration   = entry.duration();
    track.track      = entry.track;
    track.disc       = entry.disc;
    track.playCount  = entry.playCount;
    track.queued     = entry.queued();
    track.stopAfter  = entry.stopAfter;
    track.error      = entry.error;
    track.hasArtwork = !entry.artHash.empty();
    return track;
}

}  // namespace

AppPlayerControl::AppPlayerControl(PlaybackController& playback, Playlist& playlist,
                                   PlaylistView& view, AppCommands& commands,
                                   Settings& settings, RemoteJobs& jobs,
                                   Library* library, ScanStarter startScan)
    : playback_(playback),
      playlist_(playlist),
      view_(view),
      commands_(commands),
      settings_(settings),
      jobs_(jobs),
      library_(library),
      startScan_(std::move(startScan)),
      // The same generator the access token uses. It is not a secret -- it is
      // published in every status -- but it does need to differ between two runs
      // that happen to start in the same second, which a clock does not
      // guarantee.
      sessionId_(remote::generateRemoteToken().substr(0, 16)) {
    playlistWatch_ = playlist_.observe([this](const Playlist::Change&) { ++revision_; });
}

remote::Status AppPlayerControl::status() {
    remote::Status status;
    status.playing          = playback_.playing();
    status.paused           = playback_.paused();
    status.currentTrack     = playback_.currentTrack();
    status.position         = playback_.position();
    status.duration         = playback_.duration();
    status.volume           = playback_.volume();
    status.repeat           = nameOf(playlist_.repeat());
    status.shuffle          = nameOf(playlist_.shuffle());
    status.stopAfterCurrent = playlist_.stopAfterCurrent();
    status.playlistSize     = playlist_.size();
    status.playlistRevision = revision_;
    status.sessionId        = sessionId_;
    return status;
}

// Every command below asks busy() first. PlaybackController declines silently
// while a start is in flight, and answering 200 for a command that did nothing
// is the first place this API would lie.
remote::Outcome AppPlayerControl::play(std::optional<TrackId> id) {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    if (id) {
        if (playlist_.find(*id) == nullptr) {
            return Outcome::NotFound;
        }
        playback_.playTrack(*id);
        return Outcome::Ok;
    }
    if (playback_.paused()) {
        playback_.playPause();
        return Outcome::Ok;
    }
    if (playback_.playing()) {
        return Outcome::Ok;
    }
    // Nothing loaded and nothing named: start at the playlist's current entry,
    // which is what pressing Play in the window does.
    if (const std::optional<TrackId> current = playlist_.current()) {
        playback_.playTrack(*current);
        return Outcome::Ok;
    }
    if (playlist_.size() == 0) {
        return Outcome::NotFound;
    }
    playback_.playTrack(playlist_.at(0).id);
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::pause() {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    if (playback_.playing() && !playback_.paused()) {
        playback_.playPause();
    }
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::playPause() {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    playback_.playPause();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::stop() {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    playback_.stop();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::next() {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    playback_.next();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::previous() {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    playback_.previous();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::seek(double seconds) {
    if (playback_.busy()) {
        return Outcome::Busy;
    }
    if (!playback_.playing()) {
        return Outcome::Rejected;
    }
    playback_.seek(seconds);
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::setVolume(double linear) {
    playback_.setVolume(linear);
    // Through the settings key as well, so the slider and the OS's now-playing
    // entry follow -- that fan-out is Effect::Volume's job and it is reached the
    // same way the preferences pane reaches it.
    settings_.setRawValue("volume", std::to_string(linear));
    settingChanged.publish("volume");
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::setOrder(std::optional<std::string> repeat,
                                           std::optional<std::string> shuffle,
                                           std::optional<bool> stopAfterCurrent) {
    // Validated before anything is written, so a request naming one good mode
    // and one bad one changes neither.
    std::optional<RepeatMode>  repeatMode;
    std::optional<ShuffleMode> shuffleMode;
    if (repeat) {
        repeatMode = repeatFrom(*repeat);
        if (!repeatMode) {
            return Outcome::Rejected;
        }
    }
    if (shuffle) {
        shuffleMode = shuffleFrom(*shuffle);
        if (!shuffleMode) {
            return Outcome::Rejected;
        }
    }

    if (repeatMode) {
        settings_.setRawValue("repeat", std::to_string(static_cast<int>(*repeatMode)));
        settingChanged.publish("repeat");
    }
    if (shuffleMode) {
        settings_.setRawValue("shuffle", std::to_string(static_cast<int>(*shuffleMode)));
        settingChanged.publish("shuffle");
    }
    if (stopAfterCurrent) {
        settings_.setRawValue("alwaysStopAfterCurrent",
                              *stopAfterCurrent ? "true" : "false");
        settingChanged.publish("alwaysStopAfterCurrent");
    }
    return Outcome::Ok;
}

std::vector<remote::TrackSummary> AppPlayerControl::tracks(std::size_t offset,
                                                           std::size_t limit,
                                                           std::string_view query,
                                                           std::size_t& total) {
    // Over the playlist itself rather than through PlaylistView's filter: a
    // remote read must not change what the user is looking at, so the matcher is
    // used without the view's state. The sort is the view's, though, so a client
    // and the window agree on what "row 10" is.
    std::vector<remote::TrackSummary> page;
    total = 0;

    const std::size_t rows = view_.rowCount();
    for (std::size_t row = 0; row < rows; ++row) {
        const PlaylistEntry* entry = view_.entryAt(row);
        if (entry == nullptr || !playlistEntryMatches(*entry, query)) {
            continue;
        }
        const std::size_t index = total++;
        if (index >= offset && page.size() < limit) {
            page.push_back(summarise(*entry));
        }
    }
    return page;
}

std::optional<remote::TrackDetail> AppPlayerControl::track(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        return std::nullopt;
    }

    remote::TrackDetail detail;
    detail.summary      = summarise(*entry);
    detail.errorMessage = entry->errorMessage;
    detail.lyrics       = entry->unsyncedLyrics.str();
    detail.genre        = entry->genre.str();
    detail.composer     = entry->composer.str();
    detail.date         = entry->date.str();
    detail.comment      = entry->comment.str();
    detail.albumArtist  = entry->albumArtist.str();

    const TrackProperties& properties = entry->properties;
    detail.sampleRate    = properties.format.sampleRate;
    detail.channels      = static_cast<int>(properties.format.channels);
    detail.bitsPerSample = static_cast<int>(properties.format.bitsPerSample);
    detail.bitrateKbps   = properties.bitrateKbps;
    detail.seekable      = properties.seekable;
    detail.lossless      = properties.lossless;
    detail.cuesheet      = properties.cuesheet.has_value();

    // Text only. A metadata value is either a list of strings or raw bytes --
    // cover art travels in the same map -- and a picture is not something to put
    // in a JSON string. GET /playlist/{id}/artwork is where those go.
    for (const MetadataMap::Entry& tag : entry->metadata) {
        if (const auto* text = std::get_if<std::vector<std::string>>(&tag.value)) {
            std::string joined;
            for (const std::string& one : *text) {
                if (!joined.empty()) {
                    // The separator Vorbis comments already use for a repeated
                    // field, so a client splitting on it gets the list back.
                    joined += "; ";
                }
                joined += one;
            }
            detail.metadata.emplace_back(tag.key, std::move(joined));
        }
    }
    return detail;
}

std::string AppPlayerControl::addUrls(std::vector<std::string>   urls,
                                      std::optional<std::size_t> at) {
    // Handed back to the window: a scan needs the PluginCache and the
    // one-at-a-time queue MainFrame owns, and neither is this class's to hold.
    return startScan_ ? startScan_(std::move(urls), at) : std::string{};
}

remote::Outcome AppPlayerControl::removeTracks(const std::vector<TrackId>& ids) {
    return commands_.remove(ids, Origin::Remote) > 0 ? Outcome::Ok : Outcome::NotFound;
}

remote::Outcome AppPlayerControl::moveTracks(const std::vector<TrackId>& ids,
                                             TrackId                     anchor) {
    // False means the move would change nothing, which is not a failure worth
    // reporting as one.
    commands_.move(ids, anchor, Origin::Remote);
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::randomize() {
    commands_.randomize(Origin::Remote);
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::clearPlaylist() {
    commands_.clear(Origin::Remote);
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::setQueued(const std::vector<TrackId>& ids,
                                            bool                        queued) {
    return commands_.setQueued(ids, queued) > 0 ? Outcome::Ok : Outcome::NotFound;
}

remote::Outcome AppPlayerControl::clearQueue() {
    commands_.clearQueue();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::setStopAfter(const std::vector<TrackId>& ids,
                                               bool                        stopAfter) {
    return commands_.setStopAfter(ids, stopAfter) > 0 ? Outcome::Ok : Outcome::NotFound;
}

remote::Outcome AppPlayerControl::resetPlayCount(const std::vector<TrackId>& ids) {
    return commands_.resetPlayCount(ids) > 0 ? Outcome::Ok : Outcome::NotFound;
}

remote::Outcome AppPlayerControl::setRating(const std::vector<TrackId>& ids,
                                            std::optional<double>       rating) {
    if (library_ == nullptr) {
        // Ratings live only in the database. Saying so beats reporting a write
        // that had nowhere to go.
        return Outcome::Unsupported;
    }
    if (rating && *rating != 0.0) {
        for (const TrackId id : ids) {
            if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
                static_cast<void>(
                    library_->setRating(*entry, static_cast<float>(*rating)));
            }
        }
        return Outcome::Ok;
    }
    return commands_.removeRating(ids) > 0 ? Outcome::Ok : Outcome::NotFound;
}

remote::Outcome AppPlayerControl::undo() {
    commands_.undo();
    return Outcome::Ok;
}

remote::Outcome AppPlayerControl::redo() {
    commands_.redo();
    return Outcome::Ok;
}

remote::SettingInfo AppPlayerControl::describe(std::string_view key) const {
    remote::SettingInfo info;
    info.key          = std::string{key};
    info.value        = settings_.rawValue(key);
    info.defaultValue = Settings::defaultValue(key);

    const SettingEffect effect = effectOf(key);
    info.writable    = effect.effect != Effect::Internal;
    info.appliesFrom = std::string{appliesFromName(effect.applies)};

    for (const Settings::Desc& desc : Settings::all()) {
        if (desc.key == key) {
            info.type = std::string{desc.type};
            break;
        }
    }
    return info;
}

std::vector<remote::SettingInfo> AppPlayerControl::settings() {
    std::vector<remote::SettingInfo> all;
    all.reserve(Settings::all().size());
    for (const Settings::Desc& desc : Settings::all()) {
        all.push_back(describe(desc.key));
    }
    return all;
}

std::optional<remote::SettingInfo> AppPlayerControl::setting(std::string_view key) {
    for (const Settings::Desc& desc : Settings::all()) {
        if (desc.key == key) {
            return describe(key);
        }
    }
    return std::nullopt;
}

remote::SettingWrite AppPlayerControl::setSetting(std::string_view key,
                                                  std::string_view value) {
    const std::optional<remote::SettingInfo> existing = setting(key);
    if (!existing) {
        return {Outcome::NotFound, {}, "No setting with that key."};
    }
    if (!existing->writable) {
        // The rule the Advanced pane already applies to the same keys: what the
        // last session did is not a preference, and a peer does not get to
        // revise it.
        return {Outcome::ReadOnly, existing->appliesFrom,
                "That is session state rather than a setting."};
    }

    settings_.setRawValue(key, value);
    // The fan-out. Without this the value is stored and nothing acts on it until
    // the next launch -- which is exactly the bug alwaysStopAfterCurrent was.
    settingChanged.publish(std::string{key});

    return {Outcome::Ok, existing->appliesFrom, {}};
}

remote::EqualizerState AppPlayerControl::equalizer() {
    remote::EqualizerState state;
    state.enabled    = settings_.GraphicEqEnable();
    state.preamp     = settings_.EqPreamp();
    state.trackGenre = settings_.GraphicEqTrackGenre();
    state.preset     = equalizerPresetName(settings_);

    const std::span<const double>      frequencies = Equalizer::bandFrequencies();
    const std::span<const char* const> keys        = Equalizer::bandSettingsKeys();
    for (std::size_t i = 0; i < frequencies.size() && i < keys.size(); ++i) {
        double      gain = 0.0;
        const std::string raw = settings_.rawValue(keys[i]);
        try {
            gain = raw.empty() ? 0.0 : std::stod(raw);
        } catch (const std::exception&) {
            gain = 0.0;
        }
        state.bands.emplace_back(frequencies[i], gain);
    }
    return state;
}

remote::Outcome AppPlayerControl::setEqualizer(
    std::optional<bool> enabled, std::optional<double> preamp,
    const std::vector<std::pair<double, double>>& bands) {
    const std::span<const double>      frequencies = Equalizer::bandFrequencies();
    const std::span<const char* const> keys        = Equalizer::bandSettingsKeys();

    // Every band is checked against the fixed frequencies before anything is
    // written, so a request naming one band this equaliser does not have leaves
    // the curve alone rather than half-applied.
    std::vector<std::pair<std::size_t, double>> resolved;
    for (const auto& [hz, gain] : bands) {
        const auto found = std::find(frequencies.begin(), frequencies.end(), hz);
        if (found == frequencies.end()) {
            return Outcome::Rejected;
        }
        resolved.emplace_back(
            static_cast<std::size_t>(std::distance(frequencies.begin(), found)), gain);
    }

    if (enabled) {
        settings_.setRawValue("GraphicEQenable", *enabled ? "true" : "false");
    }
    if (preamp) {
        settings_.setRawValue("eqPreamp", std::to_string(*preamp));
    }
    for (const auto& [index, gain] : resolved) {
        settings_.setRawValue(keys[index], std::to_string(gain));
    }

    // A curve set by hand is no longer whichever preset the dropdown still names,
    // which is what EqualizerPanel::markCustom() says when a slider moves.
    if (!resolved.empty() || preamp) {
        markEqualizerCustom(settings_);
    }

    // Once, at the end, rather than per key: every one of them lands on the same
    // reload, and this is the key that refreshes the window's sliders too.
    settingChanged.publish("GraphicEQpreset");
    return Outcome::Ok;
}

std::vector<std::string> AppPlayerControl::equalizerPresets() {
    std::vector<std::string> names;
    for (const EqualizerPreset& preset : shippedEqualizerPresets().presets()) {
        names.push_back(preset.name);
    }
    return names;
}

remote::Outcome AppPlayerControl::applyEqualizerPreset(std::string_view name) {
    // The whole of what choosing a preset means -- the curve, the index, and
    // switching the equaliser on unless it is Flat -- lives beside the presets
    // themselves, because the window does the same thing and a policy kept in
    // two places drifts.
    if (!applyEqualizerPresetByName(settings_, name)) {
        return Outcome::NotFound;
    }

    // One announcement, on the key the panel never publishes itself. It carries
    // the DSP reload and the panel refresh together; publishing a band key would
    // reload the engine and leave the window's sliders showing the old curve.
    settingChanged.publish("GraphicEQpreset");
    return Outcome::Ok;
}

std::shared_ptr<const std::vector<std::byte>> AppPlayerControl::artwork(TrackId id) {
    if (library_ == nullptr) {
        return nullptr;
    }
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr || entry->artHash.empty()) {
        return nullptr;
    }
    // Shared rather than copied: a five-megabyte cover copied while the
    // interface thread is held would block every other request behind it.
    return library_->sharedArtwork(entry->artHash);
}

std::optional<remote::JobStatus> AppPlayerControl::job(std::string_view id) {
    return jobs_.find(id);
}

}  // namespace xpcog::app
