// The Open URL dialog's history arithmetic -- the part that decides what the
// combo box offers next time. The dialog itself needs a QApplication and a
// screen; these two helpers are pure functions, which is why they are static
// and public.

#include "windows/OpenUrlDialog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

using xpcog::app::OpenUrlDialog;

TEST_CASE("URL history round-trips through its stored form", "[app][openurl]") {
    const QStringList history = OpenUrlDialog::historyFrom(
        "http://a.example/one\nhttps://b.example/two\n");

    REQUIRE(history.size() == 2);
    CHECK(history[0] == "http://a.example/one");
    CHECK(history[1] == "https://b.example/two");

    // Empty storage is an empty history, not a list with one empty entry.
    CHECK(OpenUrlDialog::historyFrom("").isEmpty());
    // Stray blank lines -- an artefact of editing the stored value by hand --
    // do not become entries.
    CHECK(OpenUrlDialog::historyFrom("\n\nhttp://c.example/\n\n").size() == 1);
}

TEST_CASE("a reopened URL moves to the front of history instead of duplicating",
          "[app][openurl]") {
    QStringList history{"http://old.example/", "http://mid.example/",
                        "http://new.example/"};

    history = OpenUrlDialog::withUrl(history, "http://old.example/");

    REQUIRE(history.size() == 3);
    // Newest last, which is the end the combo box shows first.
    CHECK(history.last() == "http://old.example/");
    CHECK(history.first() == "http://mid.example/");
}

TEST_CASE("URL history holds fifteen entries, dropping the oldest",
          "[app][openurl]") {
    QStringList history;
    for (int i = 0; i < 20; ++i) {
        history = OpenUrlDialog::withUrl(
            history, QStringLiteral("http://station%1.example/").arg(i));
    }

    // Cog's kMaximumURLs.
    REQUIRE(history.size() == 15);
    CHECK(history.first() == "http://station5.example/");
    CHECK(history.last() == "http://station19.example/");
}
