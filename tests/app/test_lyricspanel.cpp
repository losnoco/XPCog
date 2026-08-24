// What a lyrics tag actually contains, versus what a text control can show.
//
// The panel needs a screen; this is the free function it delegates to, for the
// same reason InfoPanel's formatting is free. What it encodes is not guesswork:
// real files in a real collection carry CRLF from Windows taggers, lone CRs, and
// trailing blank lines from a copy-paste out of a lyrics site. A `\r` that
// reaches a GTK text control is drawn as a box rather than as a line break, and
// trailing blanks are indistinguishable from the song having more to say.

#include "LyricsPanel.hpp"

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
