// Everywhere that is not Windows.
//
// Not "not implemented yet" -- there is nothing here for an application to do at
// run time. Linux associations are a `.desktop` file plus shared-MIME XML
// installed into a prefix, and macOS reads CFBundleDocumentTypes out of the
// bundle's Info.plist. Both belong to whatever installs the program, and both are
// already recorded as gaps in docs/PORTING.md, alongside MPRIS having no
// DesktopEntry for the same reason.
//
// So this reports unsupported rather than succeeding silently, and the caller says
// so out loud instead of claiming to have registered something.

#include "xpcog/platform/FileAssociations.hpp"

namespace xpcog::platform {

bool fileAssociationsSupported() { return false; }

bool registerFileAssociations(const QStringList& extensions, QString* error) {
    (void)extensions;
    (void)error;
    return false;
}

bool unregisterFileAssociations(QString* error) {
    (void)error;
    return false;
}

}  // namespace xpcog::platform
