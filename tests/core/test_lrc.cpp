// LRC as real files write it, and the `.lrc` found beside a track.
//
// The parser has to decide what a lyrics tag *is* as well as read it, because
// the tag does not say: plain lyrics with a `[Chorus]` marker and a timed file
// both arrive as the same field. Those cases are the ones that matter most
// here, since getting them wrong turns a readable song into a blank pane.

#include "xpcog/core/Url.hpp"
#include "xpcog/core/lyrics/Lrc.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace xpcog;
using Catch::Approx;

namespace fs = std::filesystem;

TEST_CASE("stamps in every spelling real files use", "[lrc]") {
    const auto lyrics = parseLrc("[00:01]one\n"
                                 "[00:02.5]two\n"
                                 "[00:03.25]three\n"
                                 "[00:04.125]four\n"
                                 "[00:05:50]five\n"
                                 "[123:00.00]late\n");
    REQUIRE(lyrics);
    REQUIRE(lyrics->lines.size() == 6);
    CHECK(lyrics->lines[0].time == Approx(1.0));
    CHECK(lyrics->lines[1].time == Approx(2.5));
    CHECK(lyrics->lines[2].time == Approx(3.25));
    CHECK(lyrics->lines[3].time == Approx(4.125));
    // The colon some Windows taggers put where the dot goes.
    CHECK(lyrics->lines[4].time == Approx(5.5));
    CHECK(lyrics->lines[5].time == Approx(123.0 * 60.0));
    CHECK(lyrics->lines[0].text == "one");
}

TEST_CASE("a repeated line is written once and shown at each stamp", "[lrc]") {
    const auto lyrics = parseLrc("[00:10.00][00:30.00]chorus\n[00:20.00]verse\n");
    REQUIRE(lyrics);
    REQUIRE(lyrics->lines.size() == 3);
    CHECK(lyrics->lines[0].text == "chorus");
    CHECK(lyrics->lines[1].text == "verse");
    CHECK(lyrics->lines[2].text == "chorus");
    CHECK(lyrics->lines[2].time == Approx(30.0));
}

TEST_CASE("headers are read past, and the offset moves every line", "[lrc]") {
    // Positive offset: the words come sooner. Placed after the first line on
    // purpose -- it applies to the whole file wherever it sits.
    const auto lyrics = parseLrc("[ar:Someone]\n[ti:Something]\n"
                                 "[00:10.00]first\n[offset:+500]\n[00:20.00]second\n");
    REQUIRE(lyrics);
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].time == Approx(9.5));
    CHECK(lyrics->lines[1].time == Approx(19.5));

    // Negative, and one large enough to push a line before the start.
    const auto later = parseLrc("[offset:-250]\n[00:01.00]a\n");
    REQUIRE(later);
    CHECK(later->lines[0].time == Approx(1.25));
    const auto clamped = parseLrc("[offset:5000]\n[00:01.00]a\n");
    REQUIRE(clamped);
    CHECK(clamped->lines[0].time == 0.0);
}

TEST_CASE("word stamps are dropped, empty stamps are breaks", "[lrc]") {
    const auto lyrics =
        parseLrc("[00:01.00]<00:01.00>Hel<00:01.50>lo <00:02.00>world\r\n[00:03.00]\r\n");
    REQUIRE(lyrics);
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].text == "Hello world");
    CHECK(lyrics->lines[1].text.empty());
}

TEST_CASE("plain lyrics are not taken for a timed file", "[lrc]") {
    CHECK_FALSE(parseLrc(""));
    CHECK_FALSE(parseLrc("Just words\nand more words\n"));
    // A section marker is a bracket but not a stamp.
    CHECK_FALSE(parseLrc("[Chorus]\nla la la\n[Verse 2]\nmore\n"));
    // Headers and nothing timed: an LRC skeleton with nothing to follow.
    CHECK_FALSE(parseLrc("[ar:Someone]\n[ti:Something]\n"));
    // One stray stamp among plain lines does not make the tag followable.
    CHECK_FALSE(parseLrc("line one\n[00:10.00]line two\nline three\n"));
    // Not a stamp: seconds past 59, or letters where digits go.
    CHECK_FALSE(parseLrc("[00:75.00]no\n"));
    CHECK_FALSE(parseLrc("[aa:bb]no\n"));
}

TEST_CASE("untimed lines in a timed file are dropped", "[lrc]") {
    const auto lyrics = parseLrc("Title line\n[00:01.00]a\n[00:02.00]b\n");
    REQUIRE(lyrics);
    CHECK(lyrics->lines.size() == 2);
}

TEST_CASE("the sung line is the last one reached", "[lrc]") {
    const auto lyrics = parseLrc("[00:20.00]second\n[00:10.00]first\n[00:30.00]third\n");
    REQUIRE(lyrics);
    // Out-of-order stamps are sorted.
    CHECK(lyrics->lines[0].text == "first");

    CHECK(lyrics->lineAt(0.0) == SyncedLyrics::npos);
    CHECK(lyrics->lineAt(9.99) == SyncedLyrics::npos);
    CHECK(lyrics->lineAt(10.0) == 0);
    CHECK(lyrics->lineAt(19.0) == 0);
    CHECK(lyrics->lineAt(25.0) == 1);
    CHECK(lyrics->lineAt(3600.0) == 2);
}

TEST_CASE("plain text of a timed file", "[lrc]") {
    const auto lyrics = parseLrc("[00:00.00]\n[00:01.00]a\n[00:02.00]\n[00:03.00]b\n[00:04.00]\n");
    REQUIRE(lyrics);
    CHECK(lyrics->plainText() == "a\n\nb");
}

TEST_CASE("the .lrc beside a track", "[lrc]") {
    const fs::path dir = fs::temp_directory_path() / "xpcog-lrc-tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto write = [&](const fs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
    };

    const fs::path track = dir / "song.flac";
    write(track, "not audio");
    const Url url = Url::fromLocalPath(track);

    SECTION("none there") { CHECK_FALSE(readSidecarLrc(url)); }

    SECTION("UTF-8, with its byte-order mark dropped") {
        write(dir / "song.lrc", "\xEF\xBB\xBF[00:01.00]caf\xC3\xA9\n");
        const auto text = readSidecarLrc(url);
        REQUIRE(text);
        CHECK(*text == "[00:01.00]caf\xC3\xA9\n");
    }

    SECTION("Latin-1 is read as Latin-1") {
        write(dir / "song.lrc", "[00:01.00]caf\xE9\n");
        const auto text = readSidecarLrc(url);
        REQUIRE(text);
        CHECK(*text == "[00:01.00]caf\xC3\xA9\n");
    }

    SECTION("a track inside something else has no file of its own") {
        write(dir / "song.lrc", "[00:01.00]a\n");
        CHECK(readSidecarLrc(url.withFragment("2")) == std::nullopt);
    }

    SECTION("not for anything but a local file") {
        const auto remote = Url::parse("https://example.com/song.flac");
        REQUIRE(remote);
        CHECK_FALSE(readSidecarLrc(*remote));
    }

    fs::remove_all(dir);
}
