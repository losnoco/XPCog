#include "TagEditorModel.hpp"

#include "Translations.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace xpcog::app {
namespace {

constexpr std::string_view kAlbumArt = "albumart";

/// The alias chains, exactly PlaylistEntry::applyMetadata's, so a row seeds
/// from what a differently-tagged file actually carries -- and the dialog's
/// list of names that are not free-form matches what a save will promote
/// away. Canonical name first.
[[nodiscard]] std::span<const std::string_view> aliasesOf(TagEditorModel::Field field) {
    using Field = TagEditorModel::Field;
    static constexpr std::string_view kAlbum[]       = {"album"};
    static constexpr std::string_view kAlbumArtist[] = {"albumartist", "album artist", "album_artist"};
    static constexpr std::string_view kArtist[]      = {"artist"};
    static constexpr std::string_view kTitle[]       = {"title"};
    static constexpr std::string_view kGenre[]       = {"genre"};
    static constexpr std::string_view kComposer[]    = {"composer"};
    static constexpr std::string_view kDate[]        = {"date", "recording_date", "year"};
    static constexpr std::string_view kTrack[]       = {"tracknumber", "tracknum", "track"};
    static constexpr std::string_view kDisc[]        = {"discnumber", "discnum", "disc"};
    static constexpr std::string_view kComment[]     = {"comment"};
    static constexpr std::string_view kLyrics[]      = {"unsyncedlyrics", "unsynced lyrics", "lyrics"};
    switch (field) {
        case Field::Album:       return kAlbum;
        case Field::AlbumArtist: return kAlbumArtist;
        case Field::Artist:      return kArtist;
        case Field::Title:       return kTitle;
        case Field::Genre:       return kGenre;
        case Field::Composer:    return kComposer;
        case Field::Date:        return kDate;
        case Field::TrackNumber: return kTrack;
        case Field::DiscNumber:  return kDisc;
        case Field::Comment:     return kComment;
        case Field::Lyrics:      return kLyrics;
    }
    return {};
}

/// The write spelling: the first alias of each chain, as
/// PlaylistEntry::fullTagMap() promotes them.
[[nodiscard]] std::string_view canonicalOf(TagEditorModel::Field field) {
    return *aliasesOf(field).begin();
}

/// Every name applyMetadata promotes, losers included -- what a free row must
/// not duplicate, because applyMetadata strips all of them from `metadata`
/// and a row under a losing alias would be written and then folded away.
[[nodiscard]] bool isPromotedName(std::string_view normalizedKey) {
    static constexpr std::string_view kAll[] = {
        "album",          "albumartist",   "album artist", "album_artist",
        "artist",         "title",         "genre",        "composer",
        "date",           "recording_date", "year",        "comment",
        "tracknumber",    "tracknum",      "track",        "discnumber",
        "discnum",        "disc",          "unsyncedlyrics", "unsynced lyrics",
        "lyrics",
    };
    return std::ranges::find(kAll, normalizedKey) != std::ranges::end(kAll);
}

/// The one-line editor's separator. MetadataMap::joined's own default, so the
/// dialog reads like the playlist column does.
std::string joinValues(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

}  // namespace

TagEditorModel::TagEditorModel(std::vector<TagEditorSource> sources)
    : sources_(std::move(sources)) {
    // The merged probe starts at "everything holds" and ANDs down: what the
    // notes promise is what every file the write reaches keeps. Defaults and
    // start here disagree on purpose -- the struct's defaults say "no", the
    // merge's identity has to say "yes" or the AND never lets anything in.
    caps_.writable = caps_.arbitraryKeys = caps_.multiValue = caps_.pictures =
        caps_.chapters = !sources_.empty();
    caps_.lossyRepresentation                              = false;
    for (const TagEditorSource& source : sources_) {
        caps_.writable            = caps_.writable && source.caps.writable;
        caps_.arbitraryKeys       = caps_.arbitraryKeys && source.caps.arbitraryKeys;
        caps_.multiValue          = caps_.multiValue && source.caps.multiValue;
        caps_.pictures            = caps_.pictures && source.caps.pictures;
        caps_.chapters            = caps_.chapters && source.caps.chapters;
        caps_.lossyRepresentation = caps_.lossyRepresentation || source.caps.lossyRepresentation;
    }

    if (supportsChapters()) {
        chapters_         = sources_.front().fileChapters;
        originalChapters_ = chapters_;
    }

    for (const Field field : kFields) {
        PromotedRow& row = rowRef(field);
        for (const TagEditorSource& source : sources_) {
            // The first alias of the chain carrying a non-empty value, raw:
            // a repeated tag rides into build() still repeated, so an edit
            // elsewhere in the dialog does not silently collapse it.
            std::vector<std::string> values;
            for (const std::string_view alias : aliasesOf(field)) {
                if (const MetaValue* held = source.fileTags.find(alias); held != nullptr) {
                    if (const auto* list = std::get_if<std::vector<std::string>>(held);
                        list != nullptr && !joinValues(*list).empty()) {
                        values = *list;
                        break;
                    }
                }
            }
            // No tag under any alias: the entry's own column, which is what
            // the row would show in the playlist anyway. Not for a chaptered
            // file: its title names the whole and reads as the album, and a
            // chapter row's own title must not seed the file's Title field --
            // saving would write one chapter's name over the book's.
            const bool chaptered = source.chapterIndex >= 0 || !source.fileChapters.empty();
            if (values.empty() && !chaptered) {
                const PlaylistEntry& entry = source.entry;
                std::string          one;
                switch (field) {
                    case Field::Album:       one = entry.album.str(); break;
                    case Field::AlbumArtist: one = entry.albumArtist.str(); break;
                    case Field::Artist:      one = entry.artist.str(); break;
                    case Field::Title:       one = entry.rawTitle; break;
                    case Field::Genre:       one = entry.genre.str(); break;
                    case Field::Composer:    one = entry.composer.str(); break;
                    case Field::Date:        one = entry.date.str(); break;
                    case Field::TrackNumber:
                        one = entry.track != 0 ? std::to_string(entry.track) : std::string{};
                        break;
                    case Field::DiscNumber:
                        one = entry.disc != 0 ? std::to_string(entry.disc) : std::string{};
                        break;
                    case Field::Comment: one = entry.comment.str(); break;
                    case Field::Lyrics:  one = entry.unsyncedLyrics.str(); break;
                }
                if (!one.empty()) {
                    values = {std::move(one)};
                }
            }

            if (&source == &sources_.front()) {
                row.values = std::move(values);
            } else if (!(joinValues(row.values) == joinValues(values))) {
                // A batch where the files disagree: start blank, and let
                // `differs` say why. Typing replaces it in every file.
                row.values.clear();
                row.differs = true;
            }
        }
    }

    if (!batch()) {
        const TagEditorSource&  first = sources_.front();
        for (const MetadataMap::Entry& held : first.fileTags) {
            const auto* list = std::get_if<std::vector<std::string>>(&held.value);
            if (list == nullptr || held.key == kAlbumArt || isPromotedName(held.key)) {
                continue;
            }
            free_.push_back(FreeRow{held.key, *list});
        }

        // Embedded means the file's own bytes: fileTags is a raw read, and
        // Scanner's folder injection lives only in the entry. Art the entry
        // shows without the file carrying it is a folder cover, shown with a
        // note and never written.
        if (const auto* bytes = first.fileTags.bytes(kAlbumArt); bytes != nullptr) {
            embedded_ = *bytes;
        }
        folderArt_ = embedded_.empty() &&
                     (first.entry.metadata.bytes(kAlbumArt) != nullptr || !first.entry.artHash.empty());
    }
}

std::string TagEditorModel::label(Field field) {
    using enum Field;
    switch (field) {
        case Album:       return tr("Album");
        case AlbumArtist: return tr("Album artist");
        case Artist:      return tr("Artist");
        case Title:       return tr("Title");
        case Genre:       return tr("Genre");
        case Composer:    return tr("Composer");
        case Date:        return tr("Date");
        case TrackNumber: return tr("Track");
        case DiscNumber:  return tr("Disc");
        case Comment:     return tr("Comment");
        case Lyrics:      return tr("Lyrics");
    }
    return {};
}

std::string TagEditorModel::joined(Field field) const {
    return joinValues(values(field));
}

void TagEditorModel::setJoined(Field field, const std::string& text) {
    PromotedRow& row = rowRef(field);
    row.values       = text.empty() ? std::vector<std::string>{} : std::vector<std::string>{text};
    row.touched      = true;
    row.differs      = false;
}

std::string TagEditorModel::fieldNote(Field field) const {
    if (values(field).size() > 1 && !caps_.multiValue) {
        return tr("This format keeps only the first value.");
    }
    return {};
}

void TagEditorModel::setFreeKey(std::size_t index, std::string key) {
    if (index < free_.size()) {
        free_[index].key       = std::move(key);
        freeDirty_             = true;
    }
}

void TagEditorModel::setFreeValues(std::size_t index, std::vector<std::string> values) {
    if (index < free_.size()) {
        free_[index].values    = std::move(values);
        freeDirty_             = true;
    }
}

void TagEditorModel::addFreeRow() {
    free_.push_back(FreeRow{});
    freeDirty_ = true;
}

void TagEditorModel::removeFreeRow(std::size_t index) {
    if (index < free_.size()) {
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index));
        freeDirty_ = true;
    }
}

void TagEditorModel::moveFreeRow(std::size_t index, std::size_t to) {
    if (index < free_.size() && to < free_.size() && index != to) {
        // Whichever way the row travels, the span between the two positions
        // rotates so that the moved row lands at `to` and the rows it passed
        // shift by one -- a rotate, not an erase-and-insert, because a row may
        // be mid-edit and moving it must not disturb what the dialog holds.
        const auto begin = free_.begin();
        if (index < to) {
            std::rotate(begin + static_cast<std::ptrdiff_t>(index),
                        begin + static_cast<std::ptrdiff_t>(index) + 1,
                        begin + static_cast<std::ptrdiff_t>(to) + 1);
        } else {
            std::rotate(begin + static_cast<std::ptrdiff_t>(to),
                        begin + static_cast<std::ptrdiff_t>(index),
                        begin + static_cast<std::ptrdiff_t>(index) + 1);
        }
        freeDirty_ = true;
    }
}

std::string TagEditorModel::freeRowNote(const FreeRow& row) const {
    const std::string key = MetadataMap::normalizeKey(row.key);
    if (key.empty()) {
        return {};
    }
    std::string note;
    if (isPromotedName(key)) {
        note = tr("The field above already covers this name; it will not be written.");
    } else if (!caps_.arbitraryKeys) {
        note = tr("This format has no field for this name; it will be stored generically.");
    }
    if (row.values.size() > 1 && !caps_.multiValue) {
        if (!note.empty()) {
            note += " ";
        }
        note += tr("This format keeps only the first value.");
    }
    return note;
}

void TagEditorModel::setChapterStart(std::size_t row, std::uint64_t startMs) {
    if (row < chapters_.size()) {
        chapters_[row].startMs = startMs;
    }
}

void TagEditorModel::setChapterTitle(std::size_t row, std::string title) {
    if (row < chapters_.size()) {
        chapters_[row].title = std::move(title);
    }
}

void TagEditorModel::addChapter() {
    std::uint64_t start = 0;
    for (const Chapter& chapter : chapters_) {
        start = std::max(start, chapter.startMs + 1000);
    }
    Chapter added;
    added.startMs = start;
    chapters_.push_back(std::move(added));
}

void TagEditorModel::removeChapter(std::size_t row) {
    if (row < chapters_.size()) {
        chapters_.erase(chapters_.begin() + static_cast<std::ptrdiff_t>(row));
    }
}

int TagEditorModel::highlightedChapter() const noexcept {
    if (!supportsChapters()) {
        return -1;
    }
    const int index = sources_.front().chapterIndex;
    return index >= 0 && static_cast<std::size_t>(index) < chapters_.size() ? index : -1;
}

std::optional<ChapterList> TagEditorModel::buildChapters() const {
    if (!supportsChapters() || chapters_ == originalChapters_) {
        return std::nullopt;
    }
    return chapters_;
}

std::string TagEditorModel::timeText(std::uint64_t milliseconds) {
    const std::uint64_t hours   = milliseconds / 3600000;
    const std::uint64_t minutes = milliseconds / 60000 % 60;
    const std::uint64_t seconds = milliseconds / 1000 % 60;
    const std::uint64_t rest    = milliseconds % 1000;
    char                buffer[40];
    if (hours > 0) {
        std::snprintf(buffer, sizeof buffer, "%llu:%02llu:%02llu.%03llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds),
                      static_cast<unsigned long long>(rest));
    } else {
        std::snprintf(buffer, sizeof buffer, "%llu:%02llu.%03llu",
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds),
                      static_cast<unsigned long long>(rest));
    }
    return buffer;
}

std::optional<std::uint64_t> TagEditorModel::millisecondsFrom(std::string_view text) {
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }

    // Split on ':' keeping empty parts, so "1::2" is a rejection rather than
    // a silently shortened time.
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const std::size_t colon = text.find(':', start);
        parts.push_back(text.substr(start, colon == std::string_view::npos
                                             ? std::string_view::npos
                                             : colon - start));
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }

    // The last part is seconds, fraction allowed; the ones before it are
    // whole minutes and hours.
    const auto secondsOf = [](std::string_view part) -> std::optional<double> {
        if (part.empty()) {
            return std::nullopt;
        }
        double      value = 0.0;
        std::size_t i     = 0;
        bool        digits = false;
        while (i < part.size() && std::isdigit(static_cast<unsigned char>(part[i]))) {
            value = value * 10.0 + (part[i] - '0');
            ++i;
            digits = true;
        }
        if (i < part.size() && part[i] == '.') {
            ++i;
            double scale = 0.1;
            bool   any   = false;
            while (i < part.size() && std::isdigit(static_cast<unsigned char>(part[i]))) {
                value += (part[i] - '0') * scale;
                scale /= 10.0;
                ++i;
                any = true;
            }
            if (!any) {
                return std::nullopt;
            }
            digits = true;
        }
        if (i != part.size() || !digits) {
            return std::nullopt;
        }
        return value;
    };
    const std::optional<double> last = secondsOf(parts.back());
    if (!last || *last < 0.0) {
        return std::nullopt;
    }
    double total = *last;
    double scale = 60.0;
    for (std::size_t i = parts.size() - 1; i > 0; --i) {
        const std::string_view part = parts[i - 1];
        if (part.empty()) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (const char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
            if (value > 1000000000ULL) {
                return std::nullopt;
            }
        }
        total += static_cast<double>(value) * scale;
        scale *= 60.0;
    }
    const double milliseconds = total * 1000.0;
    if (!std::isfinite(milliseconds) ||
        milliseconds > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::llround(milliseconds));
}

bool TagEditorModel::changed() const {
    for (const Field field : kFields) {
        if (row(field).touched) {
            return true;
        }
    }
    return freeDirty_ || artAction_ != ArtAction::Keep || chapters_ != originalChapters_;
}

MetadataMap TagEditorModel::build() const {
    MetadataMap out;

    for (const Field field : kFields) {
        const std::vector<std::string>& values = row(field).values;
        // A blank field is left out entirely: the replace write's strip is
        // what deletes the old aliases then, so the dialog does not have to
        // know which of them the file carried.
        if (values.empty()) {
            continue;
        }
        out.set(canonicalOf(field), values);
    }

    for (const FreeRow& free : free_) {
        const std::string key = MetadataMap::normalizeKey(free.key);
        if (key.empty() || free.values.empty() || isPromotedName(key) || key == kAlbumArt) {
            continue;
        }
        if (out.contains(key)) {
            // Two rows the file reads the same for; their values belong
            // under the one name.
            for (const std::string& value : free.values) {
                out.add(key, value);
            }
        } else {
            out.set(key, free.values);
        }
    }

    if (!batch()) {
        // Byte entries nothing else claims -- an exotic picture block, or
        // bytes some reader found and named -- ride through untouched
        // rather than vanish under the strip.
        for (const MetadataMap::Entry& held : sources_.front().fileTags) {
            if (held.key != kAlbumArt && std::holds_alternative<std::vector<std::byte>>(held.value)) {
                out.setBytes(held.key, std::get<std::vector<std::byte>>(held.value));
            }
        }

        if (supportsArt()) {
            switch (artAction_) {
                case ArtAction::Keep:
                    // Keep must re-emit: absent means "leave alone" only to a
                    // merge writer, and this map is the whole truth to a
                    // replace one.
                    if (!embedded_.empty()) {
                        out.setBytes(kAlbumArt, embedded_);
                    }
                    break;
                case ArtAction::Remove:
                    out.setBytes(kAlbumArt, std::vector<std::byte>{});
                    break;
                case ArtAction::Replace:
                    out.setBytes(kAlbumArt, newArt_);
                    break;
            }
        }
    }

    return out;
}

MetadataMap TagEditorModel::buildChanges() const {
    MetadataMap out;
    for (const Field field : kFields) {
        const PromotedRow& row = this->row(field);
        if (!row.touched) {
            continue;
        }
        if (row.values.empty()) {
            // A cleared field still goes, with an empty value list: the
            // writer's merge deletes keys that carry one, where an omitted
            // field would mean "leave it as it is". Not a list holding an
            // empty string -- under ID3v2 that still writes a frame.
            out.set(canonicalOf(field), std::vector<std::string>{});
        } else {
            out.set(canonicalOf(field), row.values);
        }
        // The losing spellings of the name just written go with it, also as
        // empty lists. A merge write starts from TagLib's properties(), and
        // TagLib folds only the frame IDs it owns (ARTIST, TITLE): a file's
        // "album artist" or "recording_date" comes back verbatim and would
        // sit beside the canonical name this save writes. Worse for a cleared
        // field: applyMetadata reads alias chains canonical-first, so with
        // the canonical name gone the stale alias is what reads back, and
        // the clear never sticks. Names of fields nobody touched are left
        // spelled as the file had them.
        const std::string_view canonical = canonicalOf(field);
        for (const std::string_view alias : aliasesOf(field)) {
            if (alias == canonical || out.find(alias) != nullptr) {
                continue;
            }
            out.set(alias, std::vector<std::string>{});
        }
    }
    return out;
}

}  // namespace xpcog::app
