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
