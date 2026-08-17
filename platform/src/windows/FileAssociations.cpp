// Windows file associations, written through QSettings.
//
// QSettings rather than the registry API: with NativeFormat its paths *are*
// registry paths, "." names a key's default value, and it reports failure through
// status() -- which is the whole of what this needs, without windows.h in the
// middle of it. The one thing it cannot do is tell the shell something changed,
// which is what the single Win32 call at the bottom is for.
//
// What gets written, and what deliberately does not:
//
//   Software\Classes\XPCog.AudioFile              a ProgID: name, icon, command
//   Software\Classes\Applications\XPCog.exe       the "Open with" entry, plus a
//                                                 FriendlyAppName and the list of
//                                                 types this program accepts
//   Software\Classes\.<ext>\OpenWithProgids       XPCog *added* to each extension
//
// Not written: `.<ext>`'s own default value, which is the extension's current
// handler. Adding to OpenWithProgids offers XPCog; overwriting the default would
// take the file type from whatever has it. And it would not work anyway -- since
// Windows 8 the effective default lives in a hash-protected UserChoice key that
// only the Settings UI can write, so "make me the default" is not a thing an
// application can do, only a thing a user can choose.
//
// HKCU throughout, so none of this needs administrator rights, and it applies to
// the user who asked for it rather than to the machine.

#include "xpcog/platform/FileAssociations.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QString>
#include <QStringList>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

namespace xpcog::platform {
namespace {

constexpr auto kClasses = R"(HKEY_CURRENT_USER\Software\Classes)";
constexpr auto kProgId  = "XPCog.AudioFile";

/// `Applications\<basename>` -- keyed by the executable's file name, which is
/// what the shell looks up, not by its path.
QString applicationsKey() {
    return QStringLiteral("Applications/") +
           QFileInfo(QCoreApplication::applicationFilePath()).fileName();
}

/// `"C:\path\XPCog.exe" "%1"`.
///
/// Built by concatenation rather than QString::arg: the command contains a
/// literal `%1` for the shell to substitute, and arg() would replace that too --
/// producing a command line that passes the executable to itself.
QString openCommand() {
    const QString exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QLatin1Char('"') + exe + QLatin1String("\" \"%1\"");
}

bool report(QSettings& settings, QString* error) {
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        return true;
    }
    if (error != nullptr) {
        *error = settings.status() == QSettings::AccessError
                     ? QStringLiteral("the registry refused the change")
                     : QStringLiteral("the registry could not be written");
    }
    return false;
}

/// Tells the shell the association table moved. Without it Explorer keeps showing
/// the old "Open with" list until it is restarted, which reads as the
/// registration having silently failed.
void notifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
}

}  // namespace

bool fileAssociationsSupported() { return true; }

bool registerFileAssociations(const QStringList& extensions, QString* error) {
    const QString progId  = QLatin1String(kProgId);
    const QString appKey  = applicationsKey();
    const QString command = openCommand();
    const QString exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    QSettings classes(QLatin1String(kClasses), QSettings::NativeFormat);

    classes.setValue(progId + QLatin1String("/."), QStringLiteral("Audio File"));
    classes.setValue(progId + QLatin1String("/DefaultIcon/."),
                     exe + QLatin1String(",0"));
    classes.setValue(progId + QLatin1String("/shell/open/command/."), command);

    // FriendlyAppName is what "Open with" shows instead of the raw image name.
    // Worth noting for a different reason too: the SMTC card's "Unknown app"
    // caption comes from app identity rather than from this, but this is what
    // AssocQueryString(ASSOCSTR_FRIENDLYAPPNAME) answers with -- so having set it,
    // that caption is worth re-checking rather than assumed unchanged.
    classes.setValue(appKey + QLatin1String("/FriendlyAppName"),
                     QStringLiteral("XPCog"));
    classes.setValue(appKey + QLatin1String("/shell/open/command/."), command);

    for (const QString& extension : extensions) {
        const QString dotted = QLatin1Char('.') + extension;
        // Empty values: both of these are lists expressed as key names, where the
        // presence of the name is the whole content.
        classes.setValue(appKey + QLatin1String("/SupportedTypes/") + dotted,
                         QString{});
        classes.setValue(dotted + QLatin1String("/OpenWithProgids/") + progId,
                         QString{});
    }

    if (!report(classes, error)) {
        return false;
    }
    notifyShell();
    return true;
}

bool unregisterFileAssociations(QString* error) {
    const QString progId = QLatin1String(kProgId);
    const QString appKey = applicationsKey();

    QSettings classes(QLatin1String(kClasses), QSettings::NativeFormat);

    // The extension list is read back out of SupportedTypes rather than taken from
    // the caller. That way this removes exactly what was registered, even if the
    // set of built-in codecs has changed since -- an extension added by a later
    // build would otherwise be missed, and one removed would be left behind.
    classes.beginGroup(appKey + QLatin1String("/SupportedTypes"));
    const QStringList dottedExtensions = classes.childKeys();
    classes.endGroup();

    for (const QString& dotted : dottedExtensions) {
        classes.remove(dotted + QLatin1String("/OpenWithProgids/") + progId);
    }
    classes.remove(progId);
    classes.remove(appKey);

    if (!report(classes, error)) {
        return false;
    }
    notifyShell();
    return true;
}

}  // namespace xpcog::platform
