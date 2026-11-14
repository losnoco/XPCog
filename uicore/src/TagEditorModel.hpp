// The toolkit-free half of the tag editor: the rows a dialog lists, what the
// format behind them can actually hold, and what Save hands Session::editTags.
//
// Capability-driven rather than format-driven (docs/TAGEDITOR.md phase 5):
// nothing here knows the words "Vorbis" or "ID3", only the MetadataCapabilities
// the probe answered for the file in front of it. The notes that report
// folding -- "this format keeps only the first value" -- annotate rows; they
// never block a save, because a listener who wants the folding can still have
// it.
//
// Two faces:
//
//  * One file: the whole map in, the whole map out. build() is what editTags
//    gets, in replace mode -- untouched values ride through, a blank field is
//    omitted so the strip deletes it, and the cover follows the cover
//    section's choice.
//  * A batch: promoted fields only, and only the ones the listener touched
//    (buildChanges), which editTags merges into each file. Free-form rows and
//    cover art stay single-file: half a dozen files' differing extra tags do
//    not fit one table, and one chosen JPEG does not belong across a batch.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace xpcog::app {

/// What one file's rows start from: what the file itself answers to a raw
/// registry read -- folder artwork is *not* injected here, which is what makes
/// an embedded cover and a folder cover tellable apart -- and what the probe
/// says it can hold.
struct TagEditorSource {
    PlaylistEntry        entry;
    MetadataMap          fileTags;
    MetadataCapabilities caps;
};

class TagEditorModel {
public:
    /// The fields every editor shows, in the order the dialog lists them.
    /// Named for PlaylistEntry's promoted columns: saving one writes its
    /// canonical name, applyMetadata promotes it back, and the library's
    /// grouping and sorting follow.
    enum class Field {
        Album,
        AlbumArtist,
        Artist,
        Title,
        Genre,
        Composer,
        Date,
        TrackNumber,
        DiscNumber,
        Comment,
        Lyrics,
    };
    static constexpr std::array<Field, 11> kFields = {
        Field::Album,      Field::AlbumArtist, Field::Artist,   Field::Title,
        Field::Genre,      Field::Composer,    Field::Date,     Field::TrackNumber,
        Field::DiscNumber, Field::Comment,     Field::Lyrics,
    };

    /// What the cover section has chosen for the album art. The writer's
    /// convention: an `albumart` entry carrying empty bytes clears it, an
    /// absent entry leaves the file alone -- so Keep has to re-emit embedded
    /// art rather than omit it.
    enum class ArtAction { Keep, Remove, Replace };

    struct PromotedRow {
        std::vector<std::string> values;
        bool                     touched = false;
        /// Batch only: the files' values differ, so the row starts blank.
        bool differs = false;
    };

    struct FreeRow {
        /// The listener's casing; committed through
        /// MetadataMap::normalizeKey at write time.
        std::string              key;
        std::vector<std::string> values;
    };

    /// `sources` are the files behind one selection, one per distinct URL,
    /// as Session::tagEditorSources answers them. One source is the
    /// single-file face; more than one is the batch.
    explicit TagEditorModel(std::vector<TagEditorSource> sources);

    [[nodiscard]] bool batch() const noexcept { return sources_.size() > 1; }
    [[nodiscard]] const std::vector<TagEditorSource>& sources() const noexcept { return sources_; }

    /// The sources' probes merged: what every file the write reaches can do.
    /// The notes are phrased against this, which is what makes them true of
    /// each of them.
    [[nodiscard]] const MetadataCapabilities& caps() const noexcept { return caps_; }

    // --- promoted rows ------------------------------------------------------

    [[nodiscard]] static std::string label(Field field);

    [[nodiscard]] const PromotedRow& row(Field field) const {
        return rows_[static_cast<std::size_t>(field)];
    }

    /// The row's value list. Seeded from the first alias of PlaylistEntry's
    /// chain that the file carries, falling back to the entry's own column;
    /// untouched, it rides into build() verbatim, repeated values and all.
    /// A batch seeds only when every file agrees, and flags `differs` when
    /// they do not.
    [[nodiscard]] const std::vector<std::string>& values(Field field) const {
        return row(field).values;
    }

    /// The row's values joined for a one-line editor.
    [[nodiscard]] std::string joined(Field field) const;

    /// Replaces the whole list -- a typed-over multi-value tag becomes one
    /// value; that is what retyping it means. Marks the row touched.
    void setJoined(Field field, const std::string& text);

    /// True when the batch's files disagreed and the row started blank. The
    /// dialog says "the values differ"; typing replaces it in every file.
    [[nodiscard]] bool differs(Field field) const { return row(field).differs; }

    /// A row's folding note -- empty when a save keeps everything the row
    /// holds.
    [[nodiscard]] std::string fieldNote(Field field) const;

    // --- free-form rows: single file only ------------------------------------

    [[nodiscard]] bool supportsFreeRows() const noexcept { return !batch(); }

    /// The file's tags the promoted names do not cover, in the file's own
    /// order, minus album art. Empty for a batch.
    [[nodiscard]] const std::vector<FreeRow>& freeRows() const noexcept { return free_; }

    void setFreeKey(std::size_t row, std::string key);
    void setFreeValues(std::size_t row, std::vector<std::string> values);
    void addFreeRow();
    void removeFreeRow(std::size_t row);
    void moveFreeRow(std::size_t row, std::size_t to);

    /// A free row's notes: whether the format has a field of its own for the
    /// name, whether repeats survive, and whether a promoted field above
    /// already covers it -- which makes the row's write a silent no-op, so
    /// the dialog has to say it.
    [[nodiscard]] std::string freeRowNote(const FreeRow& row) const;

    // --- cover art: single file, on formats that can hold it ------------------

    [[nodiscard]] bool supportsArt() const noexcept { return !batch() && caps_.pictures; }

    /// The file's own embedded cover; empty when it carries none.
    [[nodiscard]] const std::vector<std::byte>& embeddedArt() const noexcept { return embedded_; }

    /// The entry shows art that the raw read does not: Scanner injected it
    /// from a file in the folder. The dialog shows it with a note and never
    /// writes it back into the file.
    [[nodiscard]] bool artFromFolder() const noexcept { return folderArt_; }

    [[nodiscard]] ArtAction artAction() const noexcept { return artAction_; }
    /// The chosen replacement bytes, when the action is Replace.
    [[nodiscard]] const std::vector<std::byte>& newArt() const noexcept { return newArt_; }

    void keepArt() { artAction_ = ArtAction::Keep; }
    void removeArt() { artAction_ = ArtAction::Remove; }
    void replaceArt(std::vector<std::byte> bytes) {
        newArt_     = std::move(bytes);
        artAction_  = ArtAction::Replace;
    }

    // --- what Save hands Session::editTags -------------------------------------

    /// Whether anything to write has been chosen: gates the Save button.
    [[nodiscard]] bool changed() const;

    /// The whole truth about this one file, for a replace write: promoted
    /// rows (a blank is omitted, which the strip turns into a delete), free
    /// rows in the dialog's order, byte entries nothing else claims passed
    /// through verbatim, and the cover per the cover section's action.
    [[nodiscard]] MetadataMap build() const;

    /// Only what the listener touched, for a merge write: touched promoted
    /// rows, a blank included as an empty value so the merge clears the
    /// field instead of skipping it. Nothing else -- a batch leaves each
    /// file's other tags, names, bytes and covers alone.
    [[nodiscard]] MetadataMap buildChanges() const;

private:
    [[nodiscard]] PromotedRow& rowRef(Field field) {
        return rows_[static_cast<std::size_t>(field)];
    }

    std::vector<TagEditorSource> sources_;
    MetadataCapabilities         caps_;

    /// kFields order, indices matching the enum.
    std::array<PromotedRow, kFields.size()> rows_{};

    std::vector<FreeRow> free_;
    bool                 freeDirty_ = false;

    std::vector<std::byte> embedded_;
    std::vector<std::byte> newArt_;
    bool                   folderArt_ = false;
    ArtAction              artAction_ = ArtAction::Keep;
};

}  // namespace xpcog::app
