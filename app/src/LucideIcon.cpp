#include "LucideIcon.hpp"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace xpcog::app {
namespace {

/// The sizes a toolbar, a menu and a small button ask for. Rendered rather than
/// scaled: these are vectors, and a 16px glyph rasterised from the outline is
/// sharper than a 32px one shrunk.
constexpr int kSizes[] = {16, 20, 24, 32, 48};

/// Lucide's placeholder for "the colour of the surrounding text".
constexpr auto kCurrentColor = "currentColor";

/// How much of the stroke the disabled state keeps. Qt's own disabled rendering
/// greys a pixmap towards the window colour, which on a stroked outline reads as
/// a smudge; fading it keeps the shape and loses the emphasis, which is what
/// "disabled" is meant to say.
constexpr double kDisabledOpacity = 0.35;

/// The fade is applied by the painter rather than by putting an alpha into the
/// colour. `QColor::name(QColor::HexArgb)` spells it `#AARRGGBB`, and the
/// eight-digit form SVG understands is `#RRGGBBAA` -- the same eight characters
/// in a different order, which parses without complaint as the wrong colour.
[[nodiscard]] QPixmap render(const QByteArray& svg, int size, double opacity) {
    QSvgRenderer renderer{svg};
    if (!renderer.isValid()) {
        return {};
    }

    QPixmap pixmap{size, size};
    pixmap.fill(Qt::transparent);

    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity);
    renderer.render(&painter);
    return pixmap;
}

[[nodiscard]] QIcon build(const QString& name, const QColor& colour) {
    QFile file{QStringLiteral(":/icons/lucide/%1.svg").arg(name)};
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray source = file.readAll();

    QByteArray stroked = source;
    stroked.replace(kCurrentColor, colour.name(QColor::HexRgb).toUtf8());

    QIcon icon;
    for (const int size : kSizes) {
        if (const QPixmap pixmap = render(stroked, size, 1.0); !pixmap.isNull()) {
            icon.addPixmap(pixmap, QIcon::Normal);
        }
        if (const QPixmap pixmap = render(stroked, size, kDisabledOpacity);
            !pixmap.isNull()) {
            icon.addPixmap(pixmap, QIcon::Disabled);
        }
    }
    return icon;
}

}  // namespace

QIcon lucideIcon(const QString& name, const QColor& colour) {
    // Keyed on the colour as well as the name, so switching style -- which
    // XPCog offers at run time -- produces a new set rather than serving the old
    // palette's icons for the rest of the session.
    static QHash<QString, QIcon> cache;

    const QString key = name + QLatin1Char('@') + colour.name(QColor::HexArgb);
    if (const auto found = cache.constFind(key); found != cache.constEnd()) {
        return *found;
    }
    return *cache.insert(key, build(name, colour));
}

QIcon lucideIcon(const QString& name) {
    // ButtonText rather than WindowText: almost every one of these is on a
    // toolbar button or a menu item, and on the styles that distinguish them it
    // is the one that matches the label beside it.
    //
    // The *application* palette, deliberately, and not the palette of whatever
    // widget is asking. These are rebuilt from QWidget::changeEvent on
    // QEvent::StyleChange, and at that moment the application palette already
    // holds the incoming style's colours while a widget's own palette has not
    // been re-resolved yet -- measured, on a switch into windowsvista, as
    // app=#000000 while the widget still said #ffffff. Asking the widget would
    // rebuild every icon in the colour being replaced, which is the exact bug
    // this refresh exists to fix.
    return lucideIcon(name, QApplication::palette().color(QPalette::ButtonText));
}

QStringList lucideIconNames() {
    return {
        // Transport.
        QStringLiteral("play"),
        QStringLiteral("pause"),
        QStringLiteral("square"),
        QStringLiteral("skip-back"),
        QStringLiteral("skip-forward"),
        // View menu.
        QStringLiteral("audio-lines"),
        QStringLiteral("sliders-vertical"),
        QStringLiteral("info"),
        QStringLiteral("panel-left"),
        // Elsewhere.
        QStringLiteral("folder-open"),
        QStringLiteral("x"),
    };
}

}  // namespace xpcog::app
