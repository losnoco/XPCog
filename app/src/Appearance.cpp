#include "Appearance.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QStyle>
#include <QStyleFactory>

namespace xpcog::app::appearance {
namespace {

/// The style the platform chose for itself, captured before anything overrides
/// it. Recording it is what makes "System default" reversible: once setStyle()
/// has been called there is nothing left to ask what the default *was*.
QString& platformDefault() {
    static QString key;
    return key;
}

void rememberPlatformDefault() {
    if (!platformDefault().isEmpty()) {
        return;
    }
    if (const QStyle* current = QApplication::style()) {
        platformDefault() = current->objectName();
    }
}

/// QStyleFactory's names are identifiers, not labels: `windowsvista` in a menu
/// reads like a leaked internal. Anything unrecognised falls through unchanged,
/// so a style this table has not heard of still appears and still works.
[[nodiscard]] QString labelFor(const QString& key) {
    if (key.compare(QLatin1String("windows11"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("Appearance", "Windows 11");
    }
    if (key.compare(QLatin1String("windowsvista"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("Appearance", "Windows Vista");
    }
    if (key.compare(QLatin1String("windows"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("Appearance", "Windows (classic)");
    }
    if (key.compare(QLatin1String("macos"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("Appearance", "macOS");
    }
    if (key.compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("Appearance", "Fusion");
    }
    return key;
}

}  // namespace

QList<StyleOption> availableStyles() {
    QList<StyleOption> options;
    options.append(
        StyleOption{QString{}, QCoreApplication::translate("Appearance", "System default")});

    // QStyleFactory reports the built-in styles and any style plugins found, so
    // this needs no per-platform list and cannot offer a style that is not there.
    const QStringList keys = QStyleFactory::keys();
    options.reserve(keys.size() + 1);
    for (const QString& key : keys) {
        options.append(StyleOption{key, labelFor(key)});
    }
    return options;
}

void applyStyle(const QString& key) {
    rememberPlatformDefault();

    const QString wanted = key.isEmpty() ? platformDefault() : key;
    if (wanted.isEmpty()) {
        return;
    }

    // Already correct. Worth checking: setStyle() re-polishes every widget and
    // resets the palette, so calling it needlessly is a visible flicker rather
    // than a no-op.
    if (const QStyle* current = QApplication::style();
        current != nullptr && current->objectName().compare(wanted, Qt::CaseInsensitive) == 0) {
        return;
    }

    if (QStyle* style = QStyleFactory::create(wanted)) {
        // Ownership passes to QApplication.
        QApplication::setStyle(style);
    }
}

}  // namespace xpcog::app::appearance
