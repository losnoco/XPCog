// Catch2's own main is not enough here: QCollator, and anything else that
// consults the application's locale, wants a QCoreApplication alive. Creating it
// once around the whole session is cheaper and less surprising than a fixture
// that constructs one per test case.

#include "QtStringMaker.hpp"

#include <catch2/catch_session.hpp>

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
