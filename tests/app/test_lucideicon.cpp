// That every Lucide icon the code asks for is actually in the resource.
//
// This is the failure worth a test and it is invisible: lucideIcon() answers a
// missing name with a null QIcon, and a null QIcon on a QAction draws as nothing
// at all. A renamed SVG, one dropped from the CMake FILES list, or a typo in a
// call leaves a toolbar button that is still there, still clickable, and blank.
//
// Read through QFile rather than by building the QIcon: a QIcon or a QPixmap
// needs a QGuiApplication and a platform plugin, and this binary deliberately
// has neither. What is checked instead is the substance -- the file is present
// under the name the code uses, it parses as the SVG we think it is, and it
// still carries the `currentColor` that LucideIcon.cpp rewrites. That last one
// matters most: if upstream ever ships a hard-coded stroke colour, the icons
// would silently stop following the palette and go black on a dark theme.

#include "LucideIcon.hpp"

#include "QtStringMaker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QSet>
#include <QString>

using xpcog::app::lucideIconNames;

namespace {

[[nodiscard]] QString resourcePath(const QString& name) {
    return QStringLiteral(":/icons/lucide/%1.svg").arg(name);
}

}  // namespace

TEST_CASE("every Lucide icon the app names is in the resource", "[app][icon]") {
    const QStringList names = lucideIconNames();
    // A guard against the list itself being emptied by a bad edit, which would
    // make every check below vacuously pass.
    REQUIRE(names.size() >= 11);

    for (const QString& name : names) {
        INFO("lucide icon " << name.toStdString());
        REQUIRE(QFile::exists(resourcePath(name)));

        QFile file{resourcePath(name)};
        REQUIRE(file.open(QIODevice::ReadOnly));
        const QByteArray svg = file.readAll();

        REQUIRE_FALSE(svg.isEmpty());
        CHECK(svg.contains("<svg"));
        // The hook the palette is applied through. Without it the icon still
        // draws -- in whatever colour upstream chose, which is the bug.
        CHECK(svg.contains("currentColor"));
    }
}

TEST_CASE("the icon names are unique", "[app][icon]") {
    // Two entries for one name would make the count above look healthier than
    // it is, and a duplicate is what a copy-pasted line leaves behind.
    const QStringList names = lucideIconNames();
    CHECK(QSet<QString>(names.begin(), names.end()).size() == names.size());
}
