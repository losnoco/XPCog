#include "CueSheet.hpp"

#include "../common/PlaylistText.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace xpcog::codecs {
namespace {

/// A cue sheet line is a command followed by whitespace-separated arguments,
/// where quoted arguments may contain spaces. Tokenising once is simpler than
/// Cog's per-command NSScanner juggling and behaves the same.
[[nodiscard]] std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t              i = 0;

    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }

        std::string token;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                token.push_back(line[i++]);
            }
            ++i;  // closing quote
        } else {
            while (i < line.size() &&
                   !std::isspace(static_cast<unsigned char>(line[i]))) {
                token.push_back(line[i++]);
            }
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

[[nodiscard]] std::string uppercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return out;
}

/// INDEX time is either mm:ss:ff (ff being frames of a 75 Hz CD clock) or a bare
/// sample count. Returns false for anything else.
[[nodiscard]] bool parseIndexTime(const std::string& text, double& time,
                                  bool& inSamples) {
    std::vector<std::string> parts;
    std::size_t              start = 0;
    for (;;) {
        const std::size_t colon = text.find(':', start);
        parts.push_back(text.substr(start, colon == std::string::npos
                                               ? std::string::npos
                                               : colon - start));
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }

    if (parts.size() == 1) {
        time      = std::strtod(parts[0].c_str(), nullptr);
        inSamples = true;
        return true;
    }
    if (parts.size() == 3) {
        const double minutes = std::strtod(parts[0].c_str(), nullptr);
        const double seconds = std::strtod(parts[1].c_str(), nullptr);
        const double frames  = std::strtod(parts[2].c_str(), nullptr);
        time      = (60.0 * minutes) + seconds + (frames / 75.0);
        inSamples = false;
        return true;
    }
    return false;
}

}  // namespace

MetadataMap CueTrack::metadata() const {
    MetadataMap map;
    if (!title.empty()) map.set("title", title);
    if (!artist.empty()) map.set("artist", artist);
    if (!album.empty()) map.set("album", album);
    if (!genre.empty()) map.set("genre", genre);
    if (!year.empty()) map.set("date", year);
    if (!track.empty()) map.set("tracknumber", track);
    return map;
}

CueSheet CueSheet::parse(const std::string& text, const Url& sheetUrl) {
    CueSheet sheet;

    // Album-level values come from PERFORMER/TITLE before the first FILE.
    // Track-level values come from inside a TRACK block and are reset at each
    // new TRACK.
    //
    // Deliberate divergence from Cog: Cog keeps one `artist` variable for the
    // whole sheet and never resets it, so a track-level PERFORMER leaks into
    // every following track -- one guest artist mis-credits the rest of the
    // album. The same applies to its per-track REPLAYGAIN. Fields fall back to
    // the album value here instead.
    std::string path;
    std::string trackNumber;

    std::string albumArtist;
    std::string albumTitle;
    std::string trackArtist;
    std::string trackTitle;

    std::string    genre;
    std::string    year;
    ReplayGainInfo albumGain;
    ReplayGainInfo trackGain;
    bool           trackAdded = false;
    // Set when the current TRACK is not AUDIO. Skipping the TRACK line alone is
    // not enough: its INDEX would still fire and add an entry using whatever
    // track number was left over. Cog has exactly that flaw, so a mixed-mode disc
    // gains a bogus track that decodes to noise.
    bool skipTrack = false;

    for (const std::string& line : splitLines(text)) {
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        const std::string command = uppercased(tokens[0]);

        if (command == "FILE") {
            if (tokens.size() < 2) {
                continue;
            }
            trackAdded = false;
            skipTrack  = false;
            path       = tokens[1];
        } else if (command == "TRACK") {
            trackAdded = false;
            skipTrack  = (tokens.size() < 3 || uppercased(tokens[2]) != "AUDIO");
            if (skipTrack) {
                continue;
            }
            trackNumber = tokens[1];
            trackArtist.clear();
            trackTitle.clear();
            trackGain = {};
        } else if (command == "INDEX") {
            // Only INDEX 01 starts the track; INDEX 00 is pre-gap.
            if (trackAdded || skipTrack || path.empty() || tokens.size() < 3) {
                continue;
            }
            if (std::strtol(tokens[1].c_str(), nullptr, 10) != 1) {
                continue;
            }

            double time      = 0.0;
            bool   inSamples = false;
            if (!parseIndexTime(tokens[2], time, inSamples)) {
                continue;
            }

            CueTrack entry;
            entry.track         = trackNumber.empty() ? "01" : trackNumber;
            entry.url           = resolveEntry(path, sheetUrl);
            entry.time          = time;
            entry.timeInSamples = inSamples;
            entry.artist = trackArtist.empty() ? albumArtist : trackArtist;
            entry.album  = albumTitle;
            entry.title  = trackTitle;
            entry.genre  = genre;
            entry.year   = year;

            entry.replayGain           = trackGain;
            entry.replayGain.albumGain = albumGain.albumGain;
            entry.replayGain.albumPeak = albumGain.albumPeak;

            sheet.tracks_.push_back(std::move(entry));
            trackAdded = true;
        } else if (command == "PERFORMER") {
            if (tokens.size() < 2) {
                continue;
            }
            // Before the first FILE this is the album artist; inside a TRACK
            // block it applies to that track only.
            if (path.empty()) {
                albumArtist = tokens[1];
            } else {
                trackArtist = tokens[1];
            }
        } else if (command == "TITLE") {
            if (tokens.size() < 2) {
                continue;
            }
            if (path.empty()) {
                albumTitle = tokens[1];
            } else {
                trackTitle = tokens[1];
            }
        } else if (command == "REM" && tokens.size() >= 3) {
            const std::string type = uppercased(tokens[1]);
            const std::string value = tokens[2];

            if (type == "GENRE") {
                genre = value;
            } else if (type == "DATE") {
                year = value;
            } else if (type == "REPLAYGAIN_ALBUM_GAIN") {
                albumGain.albumGain = std::strtof(value.c_str(), nullptr);
            } else if (type == "REPLAYGAIN_ALBUM_PEAK") {
                albumGain.albumPeak = std::strtof(value.c_str(), nullptr);
            } else if (type == "REPLAYGAIN_TRACK_GAIN") {
                trackGain.trackGain = std::strtof(value.c_str(), nullptr);
            } else if (type == "REPLAYGAIN_TRACK_PEAK") {
                trackGain.trackPeak = std::strtof(value.c_str(), nullptr);
            }
        }
    }

    return sheet;
}

const CueTrack* CueSheet::findTrack(std::string_view fragment) const {
    for (const CueTrack& track : tracks_) {
        if (track.track == fragment) {
            return &track;
        }
    }

    // Tolerate "3" for a sheet that writes "03", and vice versa: playlists and
    // users are inconsistent about zero padding.
    const long wanted = std::strtol(std::string{fragment}.c_str(), nullptr, 10);
    if (wanted > 0) {
        for (const CueTrack& track : tracks_) {
            if (std::strtol(track.track.c_str(), nullptr, 10) == wanted) {
                return &track;
            }
        }
    }
    return nullptr;
}

const CueTrack* CueSheet::nextInSameFile(std::size_t index) const {
    if (index + 1 >= tracks_.size()) {
        return nullptr;
    }
    const CueTrack& next = tracks_[index + 1];
    // A next track in a different file does not bound this one; the track simply
    // runs to the end of its own file.
    return (next.url == tracks_[index].url) ? &next : nullptr;
}

std::size_t CueSheet::indexOf(const CueTrack* track) const {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (&tracks_[i] == track) {
            return i;
        }
    }
    return tracks_.size();
}

}  // namespace xpcog::codecs
