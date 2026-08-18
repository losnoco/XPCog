#include "xpcog/core/library/PlaylistEntry.hpp"

#include "xpcog/core/FilePath.hpp"

#include <array>
#include <charconv>
#include <initializer_list>
#include <string_view>

namespace xpcog {
namespace {

/// Joined value for the first of `names` that carries one.
///
/// The alias chains come straight from Cog's accessors (PlaylistEntry.m:783-940):
/// tag names for the same concept differ between Vorbis comments, ID3 and APEv2,
/// and the fallback order is what makes a library tagged by several tools look
/// consistent.
///
/// Cog stops the chain on a *present* tag even when its value is empty; here an
/// empty tag falls through to the next alias, which is what a user expects from
/// a file carrying `ALBUMARTIST=` alongside a real `ALBUM ARTIST`.
[[nodiscard]] std::string firstOf(const MetadataMap&                    tags,
                                  std::initializer_list<std::string_view> names) {
    for (std::string_view name : names) {
        std::string value = tags.joined(name);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

/// Leading integer, as NSString -intValue: stops at the first non-digit, so
/// "3/12" is 3 and "1977-01-21" is 1977.
[[nodiscard]] std::int32_t leadingInt(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    bool negative = false;
    if (begin < text.size() && (text[begin] == '-' || text[begin] == '+')) {
        negative = text[begin] == '-';
        ++begin;
    }

    std::int32_t value = 0;
    std::size_t  digits = 0;
    for (std::size_t i = begin; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        // Saturate rather than overflow; a tag can hold anything.
        if (value < 100'000'000) {
            value = value * 10 + (text[i] - '0');
        }
        ++digits;
    }
    if (digits == 0) {
        return 0;
    }
    return negative ? -value : value;
}

/// Tag names promoted to columns. Anything here is removed from the map so a
/// value is never both a column and a map entry -- otherwise a writer round-trip
/// would emit it twice.
constexpr std::array kPromoted = {
    std::string_view{"album"},        std::string_view{"albumartist"},
    std::string_view{"album artist"}, std::string_view{"album_artist"},
    std::string_view{"artist"},       std::string_view{"title"},
    std::string_view{"genre"},        std::string_view{"composer"},
    std::string_view{"date"},         std::string_view{"recording_date"},
    std::string_view{"year"},         std::string_view{"comment"},
    std::string_view{"tracknumber"},  std::string_view{"tracknum"},
    std::string_view{"track"},        std::string_view{"discnumber"},
    std::string_view{"discnum"},      std::string_view{"disc"},
    std::string_view{"unsyncedlyrics"}, std::string_view{"unsynced lyrics"},
    std::string_view{"lyrics"},
};

}  // namespace

void PlaylistEntry::applyMetadata(const MetadataMap& tags) {
    album       = firstOf(tags, {"album"});
    albumArtist = firstOf(tags, {"albumartist", "album artist", "album_artist"});
    artist      = firstOf(tags, {"artist"});
    rawTitle    = firstOf(tags, {"title"});
    genre       = firstOf(tags, {"genre"});
    composer    = firstOf(tags, {"composer"});
    comment     = firstOf(tags, {"comment"});
    date        = firstOf(tags, {"date", "recording_date", "year"});
    unsyncedLyrics =
        firstOf(tags, {"unsyncedlyrics", "unsynced lyrics", "lyrics"});

    track = leadingInt(firstOf(tags, {"tracknumber", "tracknum", "track"}));
    disc  = leadingInt(firstOf(tags, {"discnumber", "discnum", "disc"}));
    year  = leadingInt(date);

    metadata.mergeFrom(tags);
    for (std::string_view name : kPromoted) {
        metadata.remove(name);
    }

    metadataLoaded = true;
}

std::string PlaylistEntry::title() const {
    if (!rawTitle.empty()) {
        return rawTitle;
    }
    return filename();
}

std::string PlaylistEntry::display() const {
    if (artist.empty()) {
        return title();
    }
    return artist + " - " + title();
}

std::string PlaylistEntry::filename() const {
    std::string name;
    if (const auto path = url.localPath()) {
        name = pathToUtf8(path->filename());
    } else {
        const std::string text = url.withoutFragment().toString();
        const std::size_t slash = text.find_last_of('/');
        name = (slash == std::string::npos) ? text : text.substr(slash + 1);
    }

    const std::string_view fragment = url.fragment();
    if (!fragment.empty()) {
        name += '#';
        name += fragment;
    }
    return name;
}

}  // namespace xpcog
