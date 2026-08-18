// XPCog's entry point.
//
// Everything the application owns is constructed here and passed down by
// reference: the codec registry, the settings store, the settings facade. Cog
// reaches for singletons (kPersistentContainer, AudioPlayer's class methods,
// NSUserDefaults) from anywhere; keeping ownership visible at the top is what
// lets the same objects be swapped for test doubles below it.

#include "AppIcon.hpp"
#include "Appearance.hpp"
#include "SingleInstance.hpp"
#include "StatusPresence.hpp"
#include "windows/MainWindow.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/platform/FileAssociations.hpp"
#include "xpcog/platform/QSettingsStore.hpp"

#include <QApplication>
#include <QMessageBox>
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

/// Whether this launch is about registration rather than about playing anything.
bool wantsRegistration(const QStringList& arguments) {
    return arguments.contains(QStringLiteral("--register")) ||
           arguments.contains(QStringLiteral("--unregister"));
}

/// Adds or removes the file associations, then reports what happened.
///
/// Runs before the single-instance handshake on purpose: registering is about this
/// installation, not about playback, and handing the flag to an already-running
/// player would leave it trying to open a file called "--register". Any file
/// arguments alongside the flag are ignored for the same reason.
///
/// The extension list comes from the codec registry, so it is exactly what this
/// build accepts. A list written out by hand here, or in an installer script,
/// would be wrong the first time a decoder was added and would stay wrong quietly.
void performRegistration(const QStringList&           arguments,
                         const xpcog::PluginRegistry& registry) {
    const bool wantsUnregister = arguments.contains(QStringLiteral("--unregister"));

    const auto complain = [](const QString& text) {
        QMessageBox::warning(nullptr, QStringLiteral("XPCog"), text);
    };

    if (!xpcog::platform::fileAssociationsSupported()) {
        complain(QObject::tr(
            "File associations are handled by the package on this platform, "
            "not by the application."));
        return;
    }

    QString error;
    if (wantsUnregister) {
        if (!xpcog::platform::unregisterFileAssociations(&error)) {
            complain(QObject::tr("Could not remove the file associations: %1").arg(error));
            return;
        }
        QMessageBox::information(nullptr, QStringLiteral("XPCog"),
                                 QObject::tr("XPCog is no longer offered for audio files."));
        return;
    }

    QStringList extensions;
    for (const std::string& extension : registry.allExtensions()) {
        extensions.append(QString::fromStdString(extension));
    }
    if (!xpcog::platform::registerFileAssociations(extensions, &error)) {
        complain(QObject::tr("Could not register the file associations: %1").arg(error));
        return;
    }

    // Explicit about what this did and did not do. Windows will not let an
    // application make itself the default handler, so promising that the next
    // double-click opens XPCog would be a promise the OS breaks.
    QMessageBox::information(
        nullptr, QStringLiteral("XPCog"),
        QObject::tr("XPCog is now offered for %n audio file type(s).\n\n"
                    "Windows does not let an application make itself the default. "
                    "To open these files by double-clicking, choose Open with → "
                    "Choose another app → XPCog, and tick \"Always use this app\".",
                    nullptr, static_cast<int>(extensions.size())));
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

    // Once, on the application: every window and dialog inherits it, so none of
    // them has to remember. Windows takes its taskbar icon from the executable's
    // own resource instead -- this is what reaches window title bars, Alt-Tab on
    // some Linux shells, and the About box.
    //
    // Not on macOS, where the same call is actively destructive and the comment
    // above used to claim otherwise. Qt implements setWindowIcon() there as
    // NSApplication.applicationIconImage, which is the Dock tile: the bundle's
    // icon shows while the app launches and is then replaced, in place, by
    // whatever is passed here. What it replaces is strictly better -- Assets.car,
    // compiled from icons/xpcog.icon, which the system draws with its container
    // and in the dark and tinted appearances -- and what it replaces it with is
    // one flat light-mode PNG. The bundle icon is right, so the fix is to leave
    // it alone rather than to pass a nicer image.
    //
    // This is invisible on Windows and Linux, and invisible in CI on all three:
    // it compiles everywhere and only a Dock shows it.
#ifndef Q_OS_MACOS
    QApplication::setWindowIcon(xpcog::app::applicationIcon());
#endif

    installTranslations();

    const QStringList arguments = QApplication::arguments();

    // Registration is not playback: it runs before the single-instance handshake,
    // and exits without ever opening a window. Its own registry, because the one
    // below is built after the handshake and this has to happen before it.
    if (wantsRegistration(arguments)) {
        xpcog::PluginRegistry registrationCodecs;
        xpcog::registerAllCodecs(registrationCodecs);
        performRegistration(arguments, registrationCodecs);
        return 0;
    }

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
    if (!instance.claim(arguments)) {
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
    // `received`, not `arguments`: these are the *other* process's command line,
    // and the enclosing scope already has ours. GCC's -Wshadow caught the second
    // one of these in as many files, which is a fair sign the name is too tempting.
    QObject::connect(&instance, &xpcog::app::SingleInstance::launched, &window,
                     [&window](const QStringList& received) {
                         const QList<QUrl> urls = urlsFromArguments(received);
                         if (!urls.isEmpty()) {
                             window.openUrls(urls);
                         }
                         xpcog::app::raiseWindow(&window);
                     });
#endif

    // Files named on the command line, so `XPCog album.cue` works and the OS can
    // hand us documents. Done after show() so the scan's progress dialog has a
    // parent window to be modal to.
    if (const QList<QUrl> opened = urlsFromArguments(arguments);
        !opened.isEmpty()) {
        window.openUrls(opened);
    }

    return QApplication::exec();
}
