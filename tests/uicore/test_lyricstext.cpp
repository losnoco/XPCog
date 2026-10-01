// What a lyrics tag actually contains, versus what a text control can show.
//
// The panel needs a screen; this is the free function it delegates to, for the
// same reason InfoPanel's formatting is free. What it encodes is not guesswork:
// real files in a real collection carry CRLF from Windows taggers, lone CRs, and
// trailing blank lines from a copy-paste out of a lyrics site. A `\r` that
// reaches a GTK text control is drawn as a box rather than as a line break, and
// trailing blanks are indistinguishable from the song having more to say.

#include "LyricsText.hpp"

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using xpcog::app::normaliseLyrics;

TEST_CASE("CRLF and lone CR both become one line break", "[lyrics]") {
    // The pair collapses to a single break rather than two, which is the bug
    // that shows as double-spaced lyrics.
    CHECK(normaliseLyrics("one\r\ntwo") == "one\ntwo");
    CHECK(normaliseLyrics("one\rtwo") == "one\ntwo");
    CHECK(normaliseLyrics("one\ntwo") == "one\ntwo");

    // Mixed within one tag, which happens when a field has been edited by two
    // different taggers.
    CHECK(normaliseLyrics("a\r\nb\rc\nd") == "a\nb\nc\nd");

    // A blank line between verses is content and must survive.
    CHECK(normaliseLyrics("verse\r\n\r\nchorus") == "verse\n\nchorus");
}

TEST_CASE("surrounding blank lines are trimmed, inner ones are not", "[lyrics]") {
    CHECK(normaliseLyrics("\n\nfirst\nlast\n\n\n") == "first\nlast");
    CHECK(normaliseLyrics("  \r\n\tsong\r\n  \r\n") == "song");

    // The inner blank is the whole point of the previous case's counterpart:
    // trimming must not reach past the ends.
    CHECK(normaliseLyrics("\n\nverse\n\nchorus\n\n") == "verse\n\nchorus");
}

TEST_CASE("a tag holding nothing readable comes back empty", "[lyrics]") {
    // Empty is what the panel branches on to show its "no lyrics" line, so a tag
    // of pure whitespace must not read as a song with a blank first page.
    CHECK(normaliseLyrics("").empty());
    CHECK(normaliseLyrics("\r\n\r\n").empty());
    CHECK(normaliseLyrics("   \t  ").empty());
}

TEST_CASE("ordinary lyrics pass through unchanged", "[lyrics]") {
    // The common case, and worth pinning: a well-formed tag must not be
    // rewritten at all.
    const std::string song = "I was sitting by the phone\nI was waiting all alone";
    CHECK(normaliseLyrics(song) == song);

    // Including non-ASCII, which is most of a real collection. The function works
    // on bytes, so a multi-byte sequence must not be split or mangled -- the
    // trimming looks for ' ', '\t' and '\n', none of which can appear as a
    // continuation byte in UTF-8.
    const std::string spanish = "Coraz\xC3\xB3n\nqu\xC3\xA9 no s\xC3\xA9";
    CHECK(normaliseLyrics(spanish) == spanish);
    CHECK(normaliseLyrics("\n" + spanish + "\n\n") == spanish);
}

TEST_CASE("timed lyrics are drawn one line per stamp, breaks and all", "[lyrics]") {
    // The highlight finds line N of the file as line N of the text, so nothing
    // may be trimmed or merged -- not the leading break, not the repeat.
    const auto lyrics =
        xpcog::parseLrc("[00:00.00]\n[00:01.00][00:05.00]chorus\n[00:03.00]verse\n[00:07.00]\n");
    REQUIRE(lyrics);
    CHECK(xpcog::app::syncedDisplayText(*lyrics) == "\nchorus\nverse\nchorus\n");
}

TEST_CASE("the sung line's colour is moved only as far as it takes to read",
          "[lyrics]") {
    using xpcog::app::readableOn;
    using xpcog::app::Rgb;
    const Rgb white{255, 255, 255};
    const Rgb black{0, 0, 0};

    // A strong accent on a light pane already reads, and is left as it is.
    const Rgb blue{0, 90, 200};
    CHECK(readableOn(blue, white, black) == blue);

    // macOS's selection pastel, which is what the line used to be drawn in:
    // against white it is nowhere near 4.5:1, so it is darkened -- but not to
    // black, because some of the tint survives before the ratio is reached.
    const Rgb pastel{179, 215, 255};
    const Rgb fixed = readableOn(pastel, white, black);
    CHECK(fixed != pastel);
    CHECK(fixed != black);
    CHECK(fixed.blue > fixed.red);

    // A dark accent on a dark pane goes the other way, towards the pane's light
    // text.
    const Rgb navy{20, 30, 90};
    const Rgb dark{30, 30, 30};
    const Rgb light{235, 235, 235};
    const Rgb lifted = readableOn(navy, dark, light);
    CHECK(lifted.red > navy.red);
    CHECK(lifted != light);
}

TEST_CASE("a timed tag is shown timed, or stripped when timing is off", "[lyrics]") {
    xpcog::PlaylistEntry entry;
    entry.artist         = "Artist";
    entry.rawTitle       = "Song";
    entry.unsyncedLyrics = "[00:01.00]first\n[00:03.00]second\n";

    xpcog::app::LyricsPresenter presenter;
    auto text = presenter.show(&entry, nullptr);
    REQUIRE(text);
    REQUIRE(text->synced);
    CHECK(text->synced->lines.size() == 2);
    CHECK(text->body == "first\nsecond");
    CHECK(text->timeable);
    // The tag is the listener's own, so no source line.
    CHECK(text->source.empty());

    // Same words, plain: a redraw, even though the body reads the same.
    presenter.setTimed(false);
    text = presenter.show(&entry, nullptr);
    REQUIRE(text);
    CHECK_FALSE(text->synced);
    CHECK(text->body == "first\nsecond");
    // Still timeable: that is what lets a pane offer to turn timing back on.
    CHECK(text->timeable);

    // And nothing to redraw when nothing changed.
    CHECK_FALSE(presenter.show(&entry, nullptr));
}

TEST_CASE("plain tag lyrics carry no timing", "[lyrics]") {
    xpcog::PlaylistEntry entry;
    entry.rawTitle       = "Song";
    entry.unsyncedLyrics = "just words\r\n";

    xpcog::app::LyricsPresenter presenter;
    const auto text = presenter.show(&entry, nullptr);
    REQUIRE(text);
    CHECK_FALSE(text->synced);
    CHECK(text->body == "just words");
    CHECK_FALSE(text->timeable);
    CHECK(text->heading == "Song");
}
