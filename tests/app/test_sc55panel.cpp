// The SC-55 panel's one dependency that can fail silently.
//
// The front-panel photograph is compiled in as a Qt resource, and a resource in
// a *static* library is exactly the kind of thing that vanishes without a word:
// the linker drops what nothing references, which is the same rule that makes
// self-registering codecs disappear (see cmake/XPCogCodec.cmake). When it goes,
// the widget draws nothing at all and looks like a synchronisation bug.
//
// No QWidget here -- this binary runs under a QCoreApplication -- and none is
// needed. What is being checked is that the bytes are reachable and are the
// shape the emulator will be handed.

#include "QtStringMaker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringList>

namespace {

/// 741 x 268 pixels of RGBA, which is what lcd_background_size comes to.
constexpr qsizetype kBackgroundBytes = 741 * 268 * 4;

}  // namespace

TEST_CASE("the SC-55 front panel image is compiled in", "[app][sc55]") {
    QFile file(QStringLiteral(":/sc55/back.data"));
    INFO("under :/sc55 -- "
         << QDir(QStringLiteral(":/sc55")).entryList().join(QLatin1String(", ")).toStdString());
    REQUIRE(file.exists());
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.size() == kBackgroundBytes);
}
