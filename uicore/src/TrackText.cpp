#include "TrackText.hpp"

#include "Translations.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/Library.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <string>

namespace xpcog::app::info {

namespace {

[[nodiscard]] std::string pad(long long value, int width) {
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < width) {
        text = "0" + text;
    }
    return text;
}

[[nodiscard]] std::string fixed(double value, int places) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
    return buffer;
}

[[nodiscard]] std::string join(const std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& line : lines) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += line;
    }
    return joined;
}

}  // namespace

std::string trackText(std::int32_t track, std::int32_t disc) {
    if (track == 0) {
        return {};
    }
    if (disc == 0) {
        return pad(track, 2);
    }
    return std::to_string(disc) + "." + pad(track, 2);
}

std::string lengthText(double seconds) {
    if (seconds <= 0.0) {
        // A live stream has no length, and "0:00.000" would be a claim rather
        // than an absence.
        return {};
    }
    const auto milliseconds = static_cast<long long>((seconds * 1000.0) + 0.5);
    return std::to_string(milliseconds / 60000) + ":" +
           pad((milliseconds / 1000) % 60, 2) + "." + pad(milliseconds % 1000, 3);
}

std::string replayGainText(const ReplayGainInfo& gain) {
    std::vector<std::string> lines;
    const auto decibels = [](float value) {
        // The sign is always shown: +0.00 dB and -0.00 dB are different claims
        // from "no gain tag", and this panel exists to tell them apart.
        return (value < 0.0F ? "" : "+") + fixed(static_cast<double>(value), 2) + " dB";
    };
    const auto peak = [](float value) { return fixed(static_cast<double>(value), 6); };

    if (gain.albumGain) {
        lines.push_back(trf("Album Gain: %s", decibels(*gain.albumGain)));
    }
    if (gain.albumPeak) {
        lines.push_back(trf("Album Peak: %s", peak(*gain.albumPeak)));
    }
    if (gain.trackGain) {
        lines.push_back(trf("Track Gain: %s", decibels(*gain.trackGain)));
    }
    if (gain.trackPeak) {
        lines.push_back(trf("Track Peak: %s", peak(*gain.trackPeak)));
    }
    if (gain.soundcheck && !gain.soundcheck->empty()) {
        lines.push_back(trf("SoundCheck: %s", *gain.soundcheck));
    }
    // Cog's condition exactly: a volume of 1.0 is no scaling and says nothing.
    if (gain.volume && *gain.volume != 1.0F) {
        // No trUtf8 twin needed here the way there is in app/src/Text.hpp: this
        // layer has no wxString to convert a literal into, so a msgid carrying
        // a multiplication sign is just bytes.
        lines.push_back(
            trf("Volume Scale: %s\xC3\x97", fixed(static_cast<double>(*gain.volume), 2)));
    }
    return join(lines);
}

std::string playCountText(std::int64_t count, std::int64_t firstSeen,
                          std::int64_t lastPlayed) {
    // wxDefaultDateTimeFormat is "%c" -- the locale's own date and time -- so
    // that is what this asks strftime for, and the wording does not change.
    const auto stamp = [](std::int64_t unixSeconds) -> std::string {
        const auto when = static_cast<std::time_t>(unixSeconds);
        std::tm    parts{};
#if defined(_WIN32)
        if (localtime_s(&parts, &when) != 0) {
            return {};
        }
#else
        if (localtime_r(&when, &parts) == nullptr) {
            return {};
        }
#endif
        std::array<char, 128> buffer{};
        const std::size_t     written =
            std::strftime(buffer.data(), buffer.size(), "%c", &parts);
        return std::string(buffer.data(), written);
    };

    std::vector<std::string> lines;
    lines.push_back(std::to_string(count));
    if (firstSeen != 0) {
        lines.push_back(trf("First seen: %s", stamp(firstSeen)));
    }
    if (lastPlayed != 0) {
        lines.push_back(trf("Last played: %s", stamp(lastPlayed)));
    }
    return join(lines);
}

std::vector<std::string> breakPieces(std::string_view text) {
    // The platform's separator, and nowhere else. A backslash is a legal
    // character in a POSIX filename, so breaking a line at one there would be
    // inventing a directory that is not in the path -- which is why this is not
    // simply "break at either slash".
    const auto separates = [](char character) {
#ifdef _WIN32
        return character == '\\' || character == '/';
#else
        return character == '/';
#endif
    };

    std::vector<std::string> pieces;
    std::string              piece;
    for (const char character : text) {
        piece += character;
        // A space is already a break wxHTML would take on its own, and counting
        // it here is what keeps the measuring below in step with what the
        // renderer will actually do with the same string.
        if (separates(character) || character == ' ') {
            pieces.push_back(piece);
            piece.clear();
        }
    }
    if (!piece.empty() || pieces.empty()) {
        pieces.push_back(piece);
    }
    return pieces;
}

const std::array<const char*, FieldCount>& fieldLabels() {
    static const std::array<const char*, FieldCount> labels = {
        XPCOG_TRANSLATE("Album Artist"), XPCOG_TRANSLATE("Artist"),
        XPCOG_TRANSLATE("Composer"),     XPCOG_TRANSLATE("Album"),
        XPCOG_TRANSLATE("Title"),        XPCOG_TRANSLATE("Track"),
        XPCOG_TRANSLATE("Length"),       XPCOG_TRANSLATE("Date"),
        XPCOG_TRANSLATE("Genre"),        XPCOG_TRANSLATE("Filename"),
        XPCOG_TRANSLATE("Sample Rate"),  XPCOG_TRANSLATE("Channels"),
        XPCOG_TRANSLATE("Bitrate"),      XPCOG_TRANSLATE("Bits"),
        XPCOG_TRANSLATE("Codec"),        XPCOG_TRANSLATE("Encoding"),
        XPCOG_TRANSLATE("Cuesheet"),     XPCOG_TRANSLATE("Replay Gain"),
        XPCOG_TRANSLATE("Play Count"),   XPCOG_TRANSLATE("Comment"),
    };
    return labels;
}

std::array<std::string, FieldCount> describe(const PlaylistEntry& entry, const Library* library) {
    std::array<std::string, FieldCount> values;

    values[AlbumArtist] = entry.albumArtist;
    values[Artist]      = entry.artist;
    values[Composer]    = entry.composer;
    values[Album]       = entry.album;
    values[Title]       = entry.title();
    values[Track]       = trackText(entry.track, entry.disc);
    values[Length]      = lengthText(entry.duration());
    // Cog binds `date`, not the year, and falls back to nothing rather than
    // inventing a January the first.
    values[Date] = entry.date.empty() && entry.year != 0 ? std::to_string(entry.year) : entry.date;
    values[Genre] = entry.genre;

    // Cog shows the last path component here. The whole path instead, because
    // this panel is resizable where Cog's fixed HUD was not, and because a path
    // you can select and copy is a large part of why an info panel gets opened.
    const auto local = entry.url.localPath();
    values[Filename] = local ? pathToUtf8(*local) : entry.url.toString();

    const TrackProperties& properties = entry.properties;
    values[SampleRate] =
        properties.format.sampleRate > 0.0
            ? std::to_string(static_cast<long long>(properties.format.sampleRate)) + " Hz"
            : std::string{};
    values[Channels] =
        properties.format.channels > 0 ? std::to_string(properties.format.channels) : std::string{};
    values[Bitrate] =
        properties.bitrateKbps > 0 ? std::to_string(properties.bitrateKbps) + " kbps" : std::string{};
    values[BitsPerSample] = properties.format.bitsPerSample > 0
                                ? std::to_string(properties.format.bitsPerSample)
                                : std::string{};
    values[Codec]    = properties.codec;
    values[Encoding] = properties.encoding;

    values[Cuesheet] =
        properties.cuesheet && !properties.cuesheet->empty() ? tr("yes") : tr("no");
    values[ReplayGain] = replayGainText(properties.replayGain);

    // The count on the entry is what the playlist carries; the dates only exist
    // in the database, so a build without one shows the count alone.
    std::int64_t firstSeen  = 0;
    std::int64_t lastPlayed = 0;
    if (library != nullptr) {
        if (const auto record = library->playCount(entry.artist, entry.album, entry.title());
            record.has_value()) {
            firstSeen  = record->firstSeen;
            lastPlayed = record->lastPlayed;
        }
    }
    values[PlayCount] = playCountText(entry.playCount, firstSeen, lastPlayed);

    values[Comment] = entry.comment;
    return values;
}

}  // namespace xpcog::app::info

namespace xpcog::app {

std::string formatClock(double seconds) {
    // Not std::lround, and not a cast of `seconds + 0.5`: see the header. A
    // negative reading is a clock that has not started, not a time before the
    // track.
    if (!(seconds > 0.0)) {  // also catches NaN, which a bad duration can be
        return "0:00";
    }

    const auto total   = static_cast<long long>(seconds);
    const long long minutes = total / 60;
    const long long rest    = total % 60;

    const auto pad = [](long long value) {
        const std::string text = std::to_string(value);
        return text.size() < 2 ? "0" + text : text;
    };

    if (minutes >= 60) {
        return std::to_string(minutes / 60) + ":" + pad(minutes % 60) + ":" + pad(rest);
    }
    return std::to_string(minutes) + ":" + pad(rest);
}

}  // namespace xpcog::app
