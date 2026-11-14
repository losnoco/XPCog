#include "TagEditorModel.hpp"

#include "Translations.hpp"

#include <algorithm>
#include <span>
#include <string_view>

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
    caps_.writable = caps_.arbitraryKeys = caps_.multiValue = caps_.pictures = !sources_.empty();
    caps_.lossyRepresentation                              = false;
    for (const TagEditorSource& source : sources_) {
        caps_.writable            = caps_.writable && source.caps.writable;
        caps_.arbitraryKeys       = caps_.arbitraryKeys && source.caps.arbitraryKeys;
        caps_.multiValue          = caps_.multiValue && source.caps.multiValue;
        caps_.pictures            = caps_.pictures && source.caps.pictures;
        caps_.lossyRepresentation = caps_.lossyRepresentation || source.caps.lossyRepresentation;
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
            // the row would show in the playlist anyway.
            if (values.empty()) {
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

bool TagEditorModel::changed() const {
    for (const Field field : kFields) {
        if (row(field).touched) {
            return true;
        }
    }
    return freeDirty_ || artAction_ != ArtAction::Keep;
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
    }
    return out;
}

}  // namespace xpcog::app
