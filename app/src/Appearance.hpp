// Choosing the widget style at run time.
//
// Qt picks a native style per platform, and on Windows it offers more than one:
// `windows11` draws the WinUI-flavoured chrome that Windows 11 apps use, while
// `windowsvista` is the older Win32 look that every Windows release from Vista to
// 10 shipped. Both are real native styles rather than themes, so switching is a
// different QStyle, not a stylesheet.
//
// This lives apart from PreferencesDialog because two callers need it and must
// not disagree: startup applies the stored style before the first window is
// built, and the dialog applies it again the moment the user changes it. One
// function, so the mapping from stored name to QStyle exists once.

#pragma once

#include <QList>
#include <QString>

namespace xpcog::app::appearance {

/// One entry per style this build can actually create, newest-looking first, led
/// by an empty key standing for the platform's own choice.
struct StyleOption {
    /// What goes in the settings store. Empty means "let the platform decide".
    QString key;
    /// Shown to the user. Translated.
    QString label;
};

[[nodiscard]] QList<StyleOption> availableStyles();

/// Switches the running application to `key`, or back to the style the platform
/// started with when `key` is empty. A name this build cannot create is ignored,
/// so a settings file written by another platform's build cannot leave the
/// application unstyled.
void applyStyle(const QString& key);

}  // namespace xpcog::app::appearance
