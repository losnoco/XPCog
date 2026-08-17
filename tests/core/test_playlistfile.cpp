// Playlist file formats: M3U, PLS, XSPF, and Cog's XML property list.
//
// The Cog XML tests are interop tests. The format is not ours, so the fixtures
// are shaped the way Foundation writes them rather than the way our writer does.

#include "xpcog/core/library/PlaylistFile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace xpcog;

namespace {

const Url kDestination = Url::fromLocalPath("/music/Pink Floyd/Animals/list.m3u");

PlaylistEntry entryAt(std::string path) {
    PlaylistEntry entry;
    entry.url = Url::fromLocalPath(std::move(path));
    return entry;
}

}  // namespace

TEST_CASE("format follows the extension", "[playlistfile]") {
    REQUIRE(playlistFormatForExtension("m3u") == PlaylistFormat::M3u);
    REQUIRE(playlistFormatForExtension("M3U8") == PlaylistFormat::M3u);
    REQUIRE(playlistFormatForExtension("pls") == PlaylistFormat::Pls);
    REQUIRE(playlistFormatForExtension("xspf") == PlaylistFormat::Xspf);
    REQUIRE(playlistFormatForExtension("xml") == PlaylistFormat::CogXml);
    REQUIRE_FALSE(playlistFormatForExtension("flac").has_value());
}

TEST_CASE("paths beside the playlist are written relative", "[playlistfile]") {
    REQUIRE(relativePathFor(Url::fromLocalPath("/music/Pink Floyd/Animals/2 Dogs.flac"),
                            kDestination) == "2 Dogs.flac");

    // Outside the playlist's directory it stays absolute. Cog never walks up
    // with "..", so a playlist cannot come to point outside its own tree.
    //
    // Compared against the URL's own path rather than a literal: on Windows a
    // POSIX-looking path is made absolute against the current drive, so the
    // literal would be testing fromLocalPath() rather than relativePathFor().
    const Url outside = Url::fromLocalPath("/music/Wish You Were Here/1.flac");
    REQUIRE(relativePathFor(outside, kDestination) ==
            outside.localPath()->generic_string());

    // A cue-track fragment survives, or every track of a cue collapses to one.
    REQUIRE(relativePathFor(
                Url::fromLocalPath("/music/Pink Floyd/Animals/Animals.flac")
                    .withFragment("3"),
                kDestination) == "Animals.flac#3");

    // Remote entries have no local path to make relative.
    const Url stream = *Url::parse("http://example.org/stream.ogg");
    REQUIRE(relativePathFor(stream, kDestination) == "http://example.org/stream.ogg");
}

TEST_CASE("M3U round-trips", "[playlistfile]") {
    const std::vector<PlaylistEntry> entries = {
        entryAt("/music/Pink Floyd/Animals/1 Pigs on the Wing 1.flac"),
        entryAt("/music/Pink Floyd/Animals/2 Dogs.flac"),
    };

    const std::string text = writePlaylist(PlaylistFormat::M3u, entries, {}, kDestination);
    REQUIRE(text == "#\n1 Pigs on the Wing 1.flac\n2 Dogs.flac\n");

    const auto read = readPlaylist(PlaylistFormat::M3u, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 2);
    REQUIRE(read->entries[1].url == entries[1].url);
}

TEST_CASE("M3U comments are metadata, not tracks", "[playlistfile]") {
    const std::string text =
        "#EXTM3U\n#EXTINF:123,Pink Floyd - Dogs\n2 Dogs.flac\n\n#comment\n";
    const auto read = readPlaylist(PlaylistFormat::M3u, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 1);
    REQUIRE(read->entries[0].url.localPath()->filename() == "2 Dogs.flac");
}

TEST_CASE("PLS round-trips and honours FileN order", "[playlistfile]") {
    const std::vector<PlaylistEntry> entries = {entryAt("/music/Pink Floyd/Animals/a.flac"),
                                                entryAt("/music/Pink Floyd/Animals/b.flac")};

    const std::string text = writePlaylist(PlaylistFormat::Pls, entries, {}, kDestination);
    REQUIRE(text.find("numberOfEntries=2") != std::string::npos);
    REQUIRE(text.find("File1=a.flac") != std::string::npos);
    REQUIRE(text.find("VERSION=2") != std::string::npos);

    // Written out of order on purpose: the number is the order, not the line.
    const auto read = readPlaylist(
        PlaylistFormat::Pls,
        "[playlist]\nFile2=b.flac\nTitle2=B\nFile1=a.flac\nVERSION=2\n", kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 2);
    REQUIRE(read->entries[0].url.localPath()->filename() == "a.flac");
    REQUIRE(read->entries[1].url.localPath()->filename() == "b.flac");
}

TEST_CASE("XSPF carries the metadata a URL list cannot", "[playlistfile]") {
    PlaylistEntry entry = entryAt("/music/Pink Floyd/Animals/2 Dogs.flac");
    entry.rawTitle      = "Dogs";
    entry.artist        = "Pink Floyd";
    entry.album         = "Animals";
    entry.track         = 2;
    entry.properties.format.sampleRate = 44100.0;
    entry.properties.totalFrames       = 44100 * 1023;

    const std::string text =
        writePlaylist(PlaylistFormat::Xspf, {entry}, {}, kDestination);

    const auto read = readPlaylist(PlaylistFormat::Xspf, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 1);

    const PlaylistEntry& back = read->entries[0];
    REQUIRE(back.url == entry.url);
    REQUIRE(back.rawTitle == "Dogs");
    REQUIRE(back.artist == "Pink Floyd");
    REQUIRE(back.album == "Animals");
    REQUIRE(back.track == 2);
    REQUIRE(back.duration() == 1023.0);
}

TEST_CASE("XSPF escapes and unescapes titles", "[playlistfile]") {
    PlaylistEntry entry = entryAt("/music/Pink Floyd/Animals/x.flac");
    entry.rawTitle      = "Us & Them <live>";

    const std::string text =
        writePlaylist(PlaylistFormat::Xspf, {entry}, {}, kDestination);
    REQUIRE(text.find("Us &amp; Them &lt;live&gt;") != std::string::npos);

    const auto read = readPlaylist(PlaylistFormat::Xspf, text, kDestination);
    REQUIRE(read->entries[0].rawTitle == "Us & Them <live>");
}

TEST_CASE("a Cog-written XML playlist reads", "[playlistfile]") {
    // Shaped as NSPropertyListSerialization writes it, down to the DOCTYPE.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>albumArt</key>
	<dict>
		<key>d41d8cd98f00b204e9800998ecf8427e</key>
		<data>
		Y292ZXI=
		</data>
	</dict>
	<key>items</key>
	<array>
		<dict>
			<key>URL</key>
			<string>2 Dogs.flac</string>
			<key>albumArt</key>
			<string>d41d8cd98f00b204e9800998ecf8427e</string>
			<key>bitrate</key>
			<integer>901</integer>
			<key>codec</key>
			<string>FLAC</string>
			<key>encoding</key>
			<string>lossless</string>
			<key>metadataBlob</key>
			<dict>
				<key>album</key>
				<array>
					<string>Animals</string>
				</array>
				<key>artist</key>
				<array>
					<string>Pink Floyd</string>
				</array>
				<key>title</key>
				<array>
					<string>Dogs</string>
				</array>
				<key>tracknumber</key>
				<array>
					<string>2</string>
				</array>
			</dict>
			<key>metadataLoaded</key>
			<true/>
			<key>replayGainAlbumGain</key>
			<real>-4.25</real>
			<key>sampleRate</key>
			<real>44100</real>
			<key>seekable</key>
			<true/>
			<key>totalFrames</key>
			<integer>45114300</integer>
		</dict>
		<dict>
			<key>URL</key>
			<string>3 Pigs.flac</string>
		</dict>
	</array>
	<key>queue</key>
	<array>
		<integer>1</integer>
	</array>
</dict>
</plist>
)";

    const auto read = readPlaylist(PlaylistFormat::CogXml, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 2);

    const PlaylistEntry& entry = read->entries[0];
    REQUIRE(entry.url == Url::fromLocalPath("/music/Pink Floyd/Animals/2 Dogs.flac"));
    REQUIRE(entry.album == "Animals");
    REQUIRE(entry.artist == "Pink Floyd");
    REQUIRE(entry.rawTitle == "Dogs");
    REQUIRE(entry.track == 2);
    REQUIRE(entry.properties.codec == "FLAC");
    REQUIRE(entry.properties.lossless);
    REQUIRE(entry.properties.totalFrames == 45114300);
    REQUIRE(entry.properties.replayGain.albumGain == -4.25F);

    // Cog cannot distinguish "no track gain" from 0 dB; an absent key must not
    // come back as a real 0 dB gain that then gets applied.
    REQUIRE_FALSE(entry.properties.replayGain.trackGain.has_value());

    REQUIRE(entry.artHash == "d41d8cd98f00b204e9800998ecf8427e");
    REQUIRE(read->artwork.size() == 1);
    REQUIRE(read->artwork[0].first == entry.artHash);
    REQUIRE(read->artwork[0].second.size() == 5);  // "cover"

    REQUIRE(read->queue == std::vector<std::size_t>{1});
}

TEST_CASE("a bare array of items is accepted", "[playlistfile]") {
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<array>
	<dict>
		<key>URL</key>
		<string>a.flac</string>
	</dict>
</array>
</plist>
)";
    const auto read = readPlaylist(PlaylistFormat::CogXml, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 1);
}

TEST_CASE("Cog XML round-trips through our own writer", "[playlistfile]") {
    PlaylistEntry entry = entryAt("/music/Pink Floyd/Animals/2 Dogs.flac");
    entry.album         = "Animals";
    entry.albumArtist   = "Pink Floyd";
    entry.artist        = "Pink Floyd";
    entry.rawTitle      = "Dogs";
    entry.track         = 2;
    entry.disc          = 1;
    entry.date          = "1977-01-23";
    entry.metadata.set("mood", std::vector<std::string>{"bleak", "restless"});
    entry.properties.format.sampleRate = 44100.0;
    entry.properties.totalFrames       = 45114300;
    entry.properties.codec             = "FLAC";
    entry.properties.encoding          = "lossless";
    entry.properties.replayGain.trackGain = -3.5F;
    entry.metadataLoaded = true;

    PlaylistEntry second = entryAt("/music/Pink Floyd/Animals/3 Pigs.flac");
    second.rawTitle      = "Pigs";

    const std::string text =
        writePlaylist(PlaylistFormat::CogXml, {entry, second}, {1}, kDestination);

    const auto read = readPlaylist(PlaylistFormat::CogXml, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->entries.size() == 2);

    const PlaylistEntry& back = read->entries[0];
    REQUIRE(back.url == entry.url);
    REQUIRE(back.album == "Animals");
    REQUIRE(back.albumArtist == "Pink Floyd");
    REQUIRE(back.rawTitle == "Dogs");
    REQUIRE(back.track == 2);
    REQUIRE(back.disc == 1);
    REQUIRE(back.year == 1977);
    REQUIRE(back.metadata.joined("mood") == "bleak, restless");
    REQUIRE(back.properties.totalFrames == 45114300);
    REQUIRE(back.properties.lossless);
    REQUIRE(back.properties.replayGain.trackGain == -3.5F);
    REQUIRE(read->queue == std::vector<std::size_t>{1});
}

TEST_CASE("XML entities and non-ASCII survive the plist", "[playlistfile]") {
    PlaylistEntry entry = entryAt("/music/Pink Floyd/Animals/x.flac");
    entry.rawTitle      = "Us & Them <\"live\"> — Ryūichi";

    const std::string text =
        writePlaylist(PlaylistFormat::CogXml, {entry}, {}, kDestination);
    const auto read = readPlaylist(PlaylistFormat::CogXml, text, kDestination);

    REQUIRE(read.has_value());
    REQUIRE(read->entries[0].rawTitle == "Us & Them <\"live\"> — Ryūichi");
}

TEST_CASE("embedded artwork survives base64", "[playlistfile]") {
    // Every byte value, so a broken base64 table shows up rather than hiding in
    // the printable range.
    std::vector<std::byte> image;
    for (int i = 0; i < 256; ++i) {
        image.push_back(static_cast<std::byte>(i));
    }

    PlaylistEntry entry = entryAt("/music/Pink Floyd/Animals/x.flac");
    entry.artHash       = "cafebabe";

    struct Context {
        std::vector<std::byte> image;
    } context{image};

    const std::string text = writePlaylist(
        PlaylistFormat::CogXml, {entry}, {}, kDestination,
        [](std::string_view hash, void* raw) -> std::vector<std::byte> {
            return (hash == "cafebabe") ? static_cast<Context*>(raw)->image
                                        : std::vector<std::byte>{};
        },
        &context);

    const auto read = readPlaylist(PlaylistFormat::CogXml, text, kDestination);
    REQUIRE(read.has_value());
    REQUIRE(read->artwork.size() == 1);
    REQUIRE(read->artwork[0].second == image);
    REQUIRE(read->entries[0].artHash == "cafebabe");
}

TEST_CASE("base64 handles every tail length", "[playlistfile]") {
    // 1, 2 and 3 trailing bytes are the three padding cases.
    for (std::size_t size : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                             std::size_t{4}, std::size_t{5}, std::size_t{6}}) {
        std::vector<std::byte> data;
        for (std::size_t i = 0; i < size; ++i) {
            data.push_back(static_cast<std::byte>(0xF0 + i));
        }

        PlaylistEntry entry = entryAt("/music/x.flac");
        entry.artHash       = "h";

        const std::string text = writePlaylist(
            PlaylistFormat::CogXml, {entry}, {}, Url::fromLocalPath("/music/list.xml"),
            [](std::string_view, void* raw) -> std::vector<std::byte> {
                return *static_cast<std::vector<std::byte>*>(raw);
            },
            &data);

        const auto read =
            readPlaylist(PlaylistFormat::CogXml, text, Url::fromLocalPath("/music/list.xml"));
        REQUIRE(read.has_value());
        REQUIRE(read->artwork.size() == 1);
        REQUIRE(read->artwork[0].second == data);
    }
}

TEST_CASE("a malformed plist is rejected rather than half-read", "[playlistfile]") {
    REQUIRE_FALSE(
        readPlaylist(PlaylistFormat::CogXml, "not xml at all", kDestination).has_value());
    REQUIRE_FALSE(
        readPlaylist(PlaylistFormat::CogXml, "<plist><dict><key>items</key>",
                     kDestination)
            .has_value());
}
