// The single-instance handshake.
//
// Worth testing rather than eyeballing because both halves fail quietly. A
// claim() that wrongly returns true gives two players fighting over the audio
// device and the library, which looks like a bug in playback rather than in
// startup; and a handover that loses the argument list gives a second launch
// that raises the window and forgets the file the user double-clicked, which
// looks like the file failed to open.
//
// Every case names its own socket. The default name is "XPCog, for this user"
// and would collide with a running player, so a test using it would pass or
// fail depending on whether the developer happens to have the app open.

#include "SingleInstance.hpp"

#include "QtStringMaker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>

#include <future>

using xpcog::app::SingleInstance;

namespace {

/// Pumps the event loop until `arrived` or the deadline passes. A plain
/// processEvents() is not enough: the payload crosses a socket, so the delivery
/// takes at least one round trip through the loop.
bool pump(const bool& arrived, int milliseconds = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!arrived && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return arrived;
}

/// Claims on another thread, because two instances in one process are not two
/// processes.
///
/// claim() blocks until the receiver acknowledges -- deliberately, since the
/// real sender is a process about to exit and must not do so with the message
/// still in the pipe. The receiver acknowledges from its event loop, and here
/// that loop belongs to the thread calling this. Running both on one thread
/// deadlocks until the timeout and then delivers nothing, which would be a fact
/// about the test rather than about the code.
std::future<bool> relaunch(SingleInstance& instance, const QStringList& arguments) {
    return std::async(std::launch::async, [&instance, arguments] {
        return instance.claim(arguments);
    });
}

}  // namespace

TEST_CASE("the first instance claims the name and a second one does not", "[app][instance]") {
    SingleInstance first{QStringLiteral("XPCog-test-claim")};
    REQUIRE(first.claim({QStringLiteral("XPCog")}));

    SingleInstance second{QStringLiteral("XPCog-test-claim")};
    REQUIRE_FALSE(second.claim({QStringLiteral("XPCog")}));
}

TEST_CASE("a later launch hands its file arguments over intact", "[app][instance]") {
    SingleInstance first{QStringLiteral("XPCog-test-handover")};
    REQUIRE(first.claim({QStringLiteral("XPCog")}));

    QStringList received;
    bool        arrived = false;
    QObject::connect(&first, &SingleInstance::launched, &first,
                     [&](const QStringList& arguments) {
                         received = arguments;
                         arrived  = true;
                     });

    // A space and a non-ASCII character, because the payload is serialised
    // rather than passed: a byte-oriented shortcut would survive plain ASCII and
    // corrupt exactly the filenames a music library is full of.
    const QStringList sent{QStringLiteral("XPCog"),
                           QStringLiteral("C:/Music/Sigur Rós/Ágætis byrjun.flac"),
                           QStringLiteral("second file.cue")};

    SingleInstance second{QStringLiteral("XPCog-test-handover")};
    std::future<bool> claimed = relaunch(second, sent);

    REQUIRE(pump(arrived));
    REQUIRE_FALSE(claimed.get());
    REQUIRE(received == sent);
}

TEST_CASE("a later launch with no files still announces itself", "[app][instance]") {
    // The window has to come forward on a bare relaunch, so the signal must fire
    // with an empty list rather than being suppressed as "nothing to do".
    SingleInstance first{QStringLiteral("XPCog-test-bare")};
    REQUIRE(first.claim({QStringLiteral("XPCog")}));

    bool arrived = false;
    QObject::connect(&first, &SingleInstance::launched, &first,
                     [&](const QStringList&) { arrived = true; });

    SingleInstance second{QStringLiteral("XPCog-test-bare")};
    std::future<bool> claimed = relaunch(second, {QStringLiteral("XPCog")});

    REQUIRE(pump(arrived));
    REQUIRE_FALSE(claimed.get());
}
