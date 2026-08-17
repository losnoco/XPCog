// XPCog's entry point.
//
// Everything the application owns is constructed here and passed down by
// reference: the codec registry, the settings store, the settings facade. Cog
// reaches for singletons (kPersistentContainer, AudioPlayer's class methods,
// NSUserDefaults) from anywhere; keeping ownership visible at the top is what
// lets the same objects be swapped for test doubles below it.

#include "windows/MainWindow.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/platform/QSettingsStore.hpp"

#include <QApplication>
#include <QList>
#include <QStringList>
#include <QUrl>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("LoSnoCo"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("losno.co"));
    QCoreApplication::setApplicationName(QStringLiteral("XPCog"));

    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    xpcog::platform::QSettingsStore store;
    xpcog::Settings                 settings{store};
    settings.applyMigrations();

    xpcog::app::MainWindow window{registry, settings};
    window.show();

    // Files named on the command line, so `XPCog album.cue` works and the OS can
    // hand us documents. Done after show() so the scan's progress dialog has a
    // parent window to be modal to.
    QList<QUrl> opened;
    const QStringList arguments = QApplication::arguments();
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        opened.append(QUrl::fromLocalFile(arguments.at(i)));
    }
    if (!opened.isEmpty()) {
        window.openUrls(opened);
    }

    return QApplication::exec();
}
