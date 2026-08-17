// Translations, against a real compiled .qm.
//
// What these cover: that the catalogue is built, loads, and holds the strings
// under the contexts the code expects -- including that "ActionRegistry" and
// "xpcog::app::ActionRegistry" are two different contexts, which is the fact
// the menu titles were getting wrong.
//
// What they do not cover: whether populateMenuBar() looks up in the right one.
// That needs a QMenuBar, which needs a QApplication and a platform plugin, and
// this binary deliberately has neither. Sabotaging the call site does not fail
// these tests -- checked, rather than assumed.
//
// Asserting on specific Spanish strings does mean a translator changing one
// fails a test. That is a real cost, accepted because a context mismatch is
// invisible in every other way: it compiles, it runs, and the interface just
// stays English.

#include "QtStringMaker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QTranslator>

namespace {

/// The compiled Spanish catalogue, path supplied by CMake.
QString spanishCatalogue() { return QStringLiteral(XPCOG_QM_ES); }

}  // namespace

TEST_CASE("the Spanish catalogue is built and loadable", "[app][i18n]") {
    REQUIRE(QFileInfo::exists(spanishCatalogue()));

    QTranslator translator;
    REQUIRE(translator.load(spanishCatalogue()));
}

TEST_CASE("the two ActionRegistry contexts are distinct and both populated",
          "[app][i18n]") {
    QTranslator translator;
    REQUIRE(translator.load(spanishCatalogue()));
    REQUIRE(QCoreApplication::installTranslator(&translator));

    // "&File" is marked with QT_TRANSLATE_NOOP("ActionRegistry", ...) and so
    // lives in that context, not in "xpcog::app::ActionRegistry" where tr()
    // inside the class would look.
    CHECK(QCoreApplication::translate("ActionRegistry", "&File") ==
          QStringLiteral("Archivo"));

    // A menu *item*, which does come from tr() inside the class, so its context
    // is the qualified one. Both must work, which is the whole point: they are
    // different contexts and the code has to use the right one for each.
    CHECK(QCoreApplication::translate("xpcog::app::ActionRegistry", "&Next") ==
          QStringLiteral("Siguiente"));

    QCoreApplication::removeTranslator(&translator);
}

TEST_CASE("an untranslated string falls back to its source", "[app][i18n]") {
    QTranslator translator;
    REQUIRE(translator.load(spanishCatalogue()));
    REQUIRE(QCoreApplication::installTranslator(&translator));

    // Most of the catalogue is still unfinished, and that has to be harmless:
    // Qt returns the source text per string, so a partial translation shows a
    // partly-English interface rather than blanks.
    CHECK(QCoreApplication::translate("xpcog::app::MainWindow", "Filter") ==
          QStringLiteral("Filter"));

    QCoreApplication::removeTranslator(&translator);
}
