// That the icon resource holds what AppIcon.cpp claims it does.
//
// This is the failure worth a test, and it is invisible by inspection: Qt
// answers a request for a size it does not have by scaling one it does, and
// reports nothing. So a PNG renamed, dropped from the CMake FILES list, or
// regenerated at the wrong dimensions leaves an application that still shows an
// icon -- just a blurry one at whichever sizes went missing, which is the tray
// and the title bar, the two places nobody screenshots.
//
// Read through QImage rather than QIcon on purpose. A QIcon or QPixmap needs a
// QGuiApplication and this binary has a QCoreApplication, by design -- there is
// no platform plugin here. QImage needs neither, and the substance of the check
// is the same: applicationIcon() is a loop over exactly these paths.

#include "AppIcon.hpp"

#include "QtStringMaker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QImage>

using xpcog::app::applicationIconPath;
using xpcog::app::applicationIconSizes;

TEST_CASE("every advertised icon size is in the resource, at that size",
          "[app][icon]") {
    for (const int size : applicationIconSizes()) {
        const QString path = applicationIconPath(size);
        INFO("icon resource " << path.toStdString());
        REQUIRE(QFile::exists(path));

        const QImage image(path);
        REQUIRE_FALSE(image.isNull());
        // Exactly square at the advertised size. A resize that let the aspect
        // ratio drift would still load and still look roughly right in the About
        // box, while being subtly wrong everywhere the icon is drawn to a square.
        REQUIRE(image.width() == size);
        REQUIRE(image.height() == size);
        // The artwork is transparent outside the gear. Without an alpha channel
        // the tray and the title bar show a black or white square around it.
        REQUIRE(image.hasAlphaChannel());
    }
}

TEST_CASE("the icon set spans the sizes the small cases need", "[app][icon]") {
    // 16 is the tray and the title bar, 32 the taskbar, 256 what Explorer wants
    // for a large view. Losing the ends of the range is the regression that
    // matters: the middle sizes scale from each other tolerably, the ends do not.
    const QList<int> sizes = applicationIconSizes();
    REQUIRE(sizes.contains(16));
    REQUIRE(sizes.contains(32));
    REQUIRE(sizes.contains(256));
}
