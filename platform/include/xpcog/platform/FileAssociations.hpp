// Telling the OS this program opens audio files.
//
// Explicitly requested, never done at startup. An application that claims file
// types the first time it runs is an application that has taken something from
// whatever held them before, without asking -- so this is behind a command-line
// flag, and there is an inverse of it.
//
// The registration is *additive* on Windows and deliberately so. It adds XPCog to
// each extension's `OpenWithProgids`, which is the list behind "Open with", and
// does not touch the extension's default handler. That is partly manners and
// partly fact: since Windows 8 the default handler lives in a hash-protected
// `UserChoice` key that only the Settings UI can write, so an application cannot
// make itself the default no matter how it asks. What it can do is become
// available to be chosen, which is what this does.
//
// The extension list is not hard-coded here. It comes from PluginRegistry, which
// is the one place that knows what the built-in codecs actually accept -- a second
// list in a script or an installer would be wrong the first time a decoder was
// added and would stay wrong silently.
//
// A no-op off Windows. Linux associations are a `.desktop` file plus shared-MIME
// XML installed into the right prefix, and macOS reads `CFBundleDocumentTypes`
// out of the bundle's Info.plist -- both are packaging, done by whatever installs
// the application, not by the application at run time.

#pragma once

#include <QString>
#include <QStringList>

namespace xpcog::platform {

/// Registers this executable as an available handler for `extensions`
/// (lowercase, no leading dot).
///
/// Returns false and fills `error` when the OS refused. Succeeds trivially where
/// there is nothing to do.
bool registerFileAssociations(const QStringList& extensions, QString* error);

/// Removes what registerFileAssociations() added, leaving anything it did not
/// create alone.
bool unregisterFileAssociations(QString* error);

/// Whether this platform has an implementation at all, so a caller can say
/// "nothing to do here" rather than reporting a silent success.
[[nodiscard]] bool fileAssociationsSupported();

}  // namespace xpcog::platform
