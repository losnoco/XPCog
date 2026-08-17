// XPCog's entry point.
//
// Everything the application owns is constructed here and passed down by
// reference: the codec registry, the settings store, the settings facade. Cog
// reaches for singletons (kPersistentContainer, AudioPlayer's class methods,
// NSUserDefaults) from anywhere; keeping ownership visible at the top is what
// lets the same objects be swapped for test doubles below it.

#include "Appearance.hpp"
#include "SingleInstance.hpp"
#include "StatusPresence.hpp"
#include "windows/MainWindow.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/platform/QSettingsStore.hpp"

#include <QApplication>
#include <QLibraryInfo>
#include <QList>
#include <QLocale>
#include <QStringList>
#include <QTranslator>
#include <QUrl>

namespace {

/// Installs the interface language, and Qt's own strings for it.
///
/// Two translators, because they come from different places: XPCog's are
/// compiled into the binary as a resource by qt_add_translations, and Qt's --
/// the text of the standard dialog buttons, the file dialog, the "About Qt"
/// box -- ship with Qt. Loading only ours gives a translated menu bar above an
/// English Open dialog, which reads as a half-finished translation.
///
/// Both are leaked deliberately: a QTranslator must outlive every widget that
/// asks it for text, and the alternative is a static whose destruction order
/// against QApplication is not something to rely on.
/// The file arguments, as URLs. Shared between the command line this process
/// was given and the one a later launch hands over, so both reach openUrls()
/// having been interpreted the same way.
QList<QUrl> urlsFromArguments(const QStringList& arguments) {
    QList<QUrl> urls;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        urls.append(QUrl::fromLocalFile(arguments.at(i)));
    }
    return urls;
}

void installTranslations() {
    const QLocale locale = QLocale::system();

    auto* qt = new QTranslator;
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt);
    }

    auto* ours = new QTranslator;
    if (ours->load(locale, QStringLiteral("xpcog"), QStringLiteral("_"),
                   QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(ours);
    }
    // No else: an unavailable language is not an error. QTranslator::load falls
    // back from es_MX to es on its own, and failing that the source strings are
    // already English.
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("LoSnoCo"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("losno.co"));
    QCoreApplication::setApplicationName(QStringLiteral("XPCog"));

    installTranslations();

    // Before anything expensive is constructed. A later launch's only job is to
    // hand its files over and go, and every decoder registered or database
    // opened first is latency the user waits through for a process that is
    // about to exit -- and, in the library's case, a second connection to a
    // file the running instance already holds.
    //
    // macOS is excluded on purpose. LaunchServices already delivers a second
    // open as an event to the running app rather than starting a new process,
    // so there is no second instance to arbitrate; claiming a socket there
    // would add a failure mode to solve a problem the platform does not have.
#ifndef Q_OS_MACOS
    xpcog::app::SingleInstance instance;
    if (!instance.claim(QApplication::arguments())) {
        return 0;
    }
#endif

    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    xpcog::platform::QSettingsStore store;
    xpcog::Settings                 settings{store};
    settings.applyMigrations();

    // Before the first window exists, so the chosen style is what gets painted
    // rather than something that visibly re-styles itself a moment after launch.
    xpcog::app::appearance::applyStyle(
        QString::fromStdString(settings.WidgetStyle()));

    xpcog::app::MainWindow window{registry, settings};
    window.show();

    // A later launch: take its files and come to the front. Raising even when
    // it brought none is the point -- someone who runs the app again while it
    // is minimised is asking for the window, and a launch that appears to do
    // nothing reads as the program having failed to start.
#ifndef Q_OS_MACOS
    QObject::connect(&instance, &xpcog::app::SingleInstance::launched, &window,
                     [&window](const QStringList& arguments) {
                         const QList<QUrl> urls = urlsFromArguments(arguments);
                         if (!urls.isEmpty()) {
                             window.openUrls(urls);
                         }
                         xpcog::app::raiseWindow(&window);
                     });
#endif

    // Files named on the command line, so `XPCog album.cue` works and the OS can
    // hand us documents. Done after show() so the scan's progress dialog has a
    // parent window to be modal to.
    if (const QList<QUrl> opened = urlsFromArguments(QApplication::arguments());
        !opened.isEmpty()) {
        window.openUrls(opened);
    }

    return QApplication::exec();
}
