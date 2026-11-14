// The tag editor's rows, without a dialog in sight.
//
// TagEditorModel is where the editor's promises are kept or broken: what a row
// starts from, what a batch does when its files disagree, which notes a format
// earns, and what Save hands Session::editTags. The frontends only render it,
// so everything worth asserting about the editor is assertable here -- and the
// capability notes are the part most likely to be wrong, because they describe
// formats this file never opens.

#include "TagEditorModel.hpp"

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

using namespace xpcog;
using namespace xpcog::app;

using Field = TagEditorModel::Field;

namespace {

/// A writable Vorbis-ish file: names of its own, repeats that survive, pictures.
constexpr MetadataCapabilities kVorbis{true, true, true, true, false};

/// An MP3-ish file: generic frames for names it has no field for, repeats
/// folded, pictures it can hold.
constexpr MetadataCapabilities kId3{true, false, false, true, true};

/// A file the tag editor has no business opening at all.
constexpr MetadataCapabilities kUnwritable{};

TagEditorSource makeSource(std::string name, MetadataMap fileTags, MetadataCapabilities caps) {
    TagEditorSource source;
    source.entry.url      = Url::fromLocalPath(std::filesystem::path("/music") / name);
    source.entry.rawTitle = std::move(name);
    source.fileTags       = std::move(fileTags);
    source.caps           = caps;
    return source;
}

std::vector<std::string> valuesOf(const MetadataMap& map, std::string_view key) {
    const MetaValue* held = map.find(key);
    REQUIRE(held != nullptr);
    const auto* list = std::get_if<std::vector<std::string>>(held);
    REQUIRE(list != nullptr);
    return *list;
}

}  // namespace

TEST_CASE("rows seed from the file's tags, aliases and all", "[uicore][tageditor]") {
    MetadataMap tags;
    tags.set("album artist", std::vector<std::string>{"Someone Else"});
    tags.set("year", std::vector<std::string>{"1979"});
    tags.set("artist", std::vector<std::string>{"One", "Two"});

    TagEditorModel model{{makeSource("seeded.flac", tags, kVorbis)}};

    // An alias further down the chain still fills the row, and a canonical
    // name elsewhere in the map does not shadow it.
    CHECK(model.joined(Field::AlbumArtist) == "Someone Else");
    CHECK(model.joined(Field::Date) == "1979");

    // Repeats ride in as they are; an edit somewhere else in the dialog must
    // not collapse them on the way out.
    CHECK(model.values(Field::Artist) == std::vector<std::string>{"One", "Two"});

    // Nothing under any alias for the album: the entry's own column answers,
    // which is what the playlist row showed.
    TagEditorSource fallback = makeSource("columns.flac", {}, kVorbis);
    fallback.entry.album     = "Parallel Lines";
    fallback.entry.track     = 3;
    fallback.entry.disc      = 0;
    TagEditorModel columns{{std::move(fallback)}};
    CHECK(columns.joined(Field::Album) == "Parallel Lines");
    CHECK(columns.joined(Field::TrackNumber) == "3");
    CHECK(columns.values(Field::DiscNumber).empty());
}

TEST_CASE("a batch starts blank where its files disagree", "[uicore][tageditor]") {
    MetadataMap first;
    first.set("album", std::vector<std::string>{"Parallel Lines"});
    first.set("artist", std::vector<std::string>{"Blondie"});
    MetadataMap second;
    second.set("album", std::vector<std::string>{"Parallel Lines"});
    second.set("artist", std::vector<std::string>{"Harry Nilsson"});

    TagEditorModel model{{makeSource("one.flac", first, kVorbis),
                          makeSource("two.flac", second, kId3)}};
    REQUIRE(model.batch());

    CHECK_FALSE(model.differs(Field::Album));
    CHECK(model.joined(Field::Album) == "Parallel Lines");

    // Blank, and saying why: what the listener types replaces it in all of
    // them, which is the only answer that fits both files.
    CHECK(model.differs(Field::Artist));
    CHECK(model.values(Field::Artist).empty());

    // Typing clears the flag, and an untouched row still writes nothing.
    model.setJoined(Field::Artist, "Blondie & Harry Nilsson");
    CHECK_FALSE(model.differs(Field::Artist));
    CHECK(model.changed());

    // The merged probe: what every file reached keeps. Vorbis says yes to
    // everything; the MP3's no carries.
    CHECK(model.caps().writable);
    CHECK_FALSE(model.caps().arbitraryKeys);
    CHECK_FALSE(model.caps().multiValue);
    CHECK(model.caps().pictures);
    CHECK(model.caps().lossyRepresentation);

    // Art is single-file only: one chosen JPEG does not belong across a batch.
    CHECK_FALSE(model.supportsArt());
    CHECK(model.freeRows().empty());
}

TEST_CASE("build() is the whole truth about one file", "[uicore][tageditor]") {
    MetadataMap tags;
    tags.set("title", std::vector<std::string>{"Heart of Glass"});
    tags.set("artist", std::vector<std::string>{"Blondie"});
    tags.set("mood", std::vector<std::string>{"driving"});
    tags.set("bpm", std::vector<std::string>{"128"});
    tags.setBytes("albumart", std::vector<std::byte>{std::byte{0x89}, std::byte{0x50}});
    tags.setBytes("application/x-cue", std::vector<std::byte>{std::byte{0x01}});

    TagEditorModel model{{makeSource("whole.flac", tags, kVorbis)}};
    model.setJoined(Field::Genre, "New Wave");
    model.setJoined(Field::Album, "");

    MetadataMap out = model.build();

    // Touched and untouched rows both ride out, under the canonical name.
    CHECK(valuesOf(out, "title") == std::vector<std::string>{"Heart of Glass"});
    CHECK(valuesOf(out, "genre") == std::vector<std::string>{"New Wave"});

    // A blank field is left out of the map entirely; the replace write's strip
    // is what deletes the old aliases then.
    CHECK_FALSE(out.contains("album"));

    // Free rows keep their names and the file's order; byte entries nothing
    // else claims pass through rather than vanishing under the strip.
    CHECK(valuesOf(out, "mood") == std::vector<std::string>{"driving"});
    CHECK(valuesOf(out, "bpm") == std::vector<std::string>{"128"});
    REQUIRE(out.bytes("application/x-cue") != nullptr);
    CHECK(*out.bytes("application/x-cue") == std::vector<std::byte>{std::byte{0x01}});

    // Keep re-emits the file's own cover: absent means "leave alone" only to a
    // merge writer, and this map is the whole truth to a replace one.
    REQUIRE(out.bytes("albumart") != nullptr);
    CHECK(out.bytes("albumart")->size() == 2);

    model.removeArt();
    REQUIRE(model.build().bytes("albumart") != nullptr);
    CHECK(model.build().bytes("albumart")->empty());

    model.replaceArt(std::vector<std::byte>{std::byte{0xFF}});
    REQUIRE(model.build().bytes("albumart") != nullptr);
    CHECK(*model.build().bytes("albumart") == std::vector<std::byte>{std::byte{0xFF}});

    // A format with no room for pictures gets no art entry to be confused by.
    constexpr MetadataCapabilities kNoPictures{true, true, false, false, false};
    TagEditorModel plain{{makeSource("plain.wv", tags, kNoPictures)}};
    plain.removeArt();
    CHECK_FALSE(plain.build().contains("albumart"));
}

TEST_CASE("free rows are committed as tags, not as rows", "[uicore][tageditor]") {
    MetadataMap tags;
    tags.set("mood", std::vector<std::string>{"driving"});
    TagEditorModel model{{makeSource("free.flac", tags, kVorbis)}};
    REQUIRE(model.supportsFreeRows());
    REQUIRE(model.freeRows().size() == 1);
    CHECK(model.freeRows()[0].key == "mood");

    // A new row under a name the file reads the same for: their values belong
    // under the one key, in the file's own normalized spelling.
    model.addFreeRow();
    model.setFreeKey(1, "Mood");
    model.setFreeValues(1, {"melodic"});
    CHECK(valuesOf(model.build(), "mood") ==
          std::vector<std::string>{"driving", "melodic"});

    // A promoted name is the field above's business; a row under it would be
    // written and then promoted away, so it is not written at all. The same
    // for a blank key, a valueless row, and the art key itself.
    model.addFreeRow();
    model.setFreeKey(2, "Title");
    model.setFreeValues(2, {"wrong"});
    model.addFreeRow();
    model.setFreeKey(3, "");
    model.setFreeValues(3, {"nowhere to go"});
    model.addFreeRow();
    model.setFreeKey(4, "albumart");
    model.setFreeValues(4, {"not a string"});
    MetadataMap out = model.build();
    // The row under `Title` writes nothing: what lands there is the promoted
    // row's own value, which came from the entry's column.
    CHECK(out.first("title") == "free.flac");
    CHECK(out.size() == 2);

    // Reordering moves rows; removing drops them.
    model.removeFreeRow(4);
    model.removeFreeRow(3);
    model.removeFreeRow(2);
    model.moveFreeRow(0, 1);
    REQUIRE(model.freeRows().size() == 2);
    CHECK(model.freeRows()[0].key == "Mood");
    CHECK(model.freeRows()[1].key == "mood");
}

TEST_CASE("buildChanges() is only what the listener touched", "[uicore][tageditor]") {
    MetadataMap tags;
    tags.set("title", std::vector<std::string>{"Heart of Glass"});
    tags.set("album", std::vector<std::string>{"Parallel Lines"});
    TagEditorModel model{{makeSource("batch.flac", tags, kVorbis),
                          makeSource("batch2.flac", tags, kVorbis)}};

    CHECK_FALSE(model.changed());
    CHECK(model.buildChanges().empty());

    model.setJoined(Field::Genre, "New Wave");
    model.setJoined(Field::Album, "");

    MetadataMap changes = model.buildChanges();
    CHECK(valuesOf(changes, "genre") == std::vector<std::string>{"New Wave"});
    // A cleared field goes too, with an empty value list -- the merge writer
    // reads that as "delete this key", where an omitted field would mean
    // "leave it as it is". Not a list holding an empty string: under ID3v2
    // that still writes a frame.
    REQUIRE(changes.find("album") != nullptr);
    CHECK(std::get<std::vector<std::string>>(*changes.find("album")).empty());
    CHECK_FALSE(changes.contains("title"));

    // Free rows and cover art are never part of a batch's edit.
    CHECK_FALSE(changes.contains("mood"));
    CHECK_FALSE(changes.contains("albumart"));
}

TEST_CASE("the notes say what the format will do", "[uicore][tageditor]") {
    MetadataMap tags;
    tags.set("artist", std::vector<std::string>{"One", "Two"});
    TagEditorModel vorbis{{makeSource("vorbis.flac", tags, kVorbis)}};
    CHECK(vorbis.fieldNote(Field::Artist).empty());

    TagEditorModel mp3{{makeSource("mp3.mp3", tags, kId3)}};
    CHECK_FALSE(mp3.fieldNote(Field::Artist).empty());

    // A free row's notes: no field of its own, and only the first value kept.
    TagEditorModel::FreeRow custom{"MOOD", {"driving", "melodic"}};
    const std::string note = mp3.freeRowNote(custom);
    CHECK_FALSE(note.empty());
    CHECK(note.find("generically") != std::string::npos);
    CHECK(note.find("first value") != std::string::npos);

    // A promoted name is covered by the field above, and says so.
    TagEditorModel::FreeRow shadow{"Title", {"one"}};
    CHECK(vorbis.freeRowNote(shadow).find("already covers") != std::string::npos);

    // A row with no name yet has nothing to comment on.
    CHECK(vorbis.freeRowNote(TagEditorModel::FreeRow{}).empty());

    // A format that holds everything has nothing to warn about.
    CHECK(vorbis.freeRowNote(custom).empty());
}

TEST_CASE("an embedded cover and a folder cover tell each other apart", "[uicore][tageditor]") {
    const std::vector<std::byte> picture{std::byte{0x89}, std::byte{0x50}};

    MetadataMap embedded;
    embedded.setBytes("albumart", picture);
    TagEditorModel inFile{{makeSource("embedded.flac", embedded, kVorbis)}};
    CHECK(inFile.embeddedArt() == picture);
    CHECK_FALSE(inFile.artFromFolder());
    CHECK(inFile.supportsArt());

    // Scanner put the entry's art there from a file in the folder; the raw
    // read the model was handed does not carry it, and that is the whole test.
    TagEditorSource fromFolder = makeSource("folder.flac", {}, kVorbis);
    fromFolder.entry.metadata.setBytes("albumart", picture);
    fromFolder.entry.artHash     = "sha256-of-something";
    TagEditorModel folder{{std::move(fromFolder)}};
    CHECK(folder.embeddedArt().empty());
    CHECK(folder.artFromFolder());

    // A format with no pictures section has no cover row at all.
    TagEditorSource audio = makeSource("artless.wav", embedded, kUnwritable);
    TagEditorModel none{{std::move(audio)}};
    CHECK_FALSE(none.supportsArt());
    CHECK_FALSE(none.caps().writable);
}
