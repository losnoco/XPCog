#include "AppIcon.hpp"

#include <QPixmap>
#include <QString>

namespace xpcog::app {
namespace {

/// Kept in step with PNG_SIZES in app/icons/make-icons.py by the test that walks
/// this list against the resource. Two lists in two languages is not ideal, but
/// the alternative is generating a header from the script, which makes the build
/// depend on ImageMagick to compile.
constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};

}  // namespace

QIcon applicationIcon() {
    static const QIcon icon = [] {
        QIcon built;
        for (const int size : kSizes) {
            const QPixmap pixmap(applicationIconPath(size));
            if (!pixmap.isNull()) {
                built.addPixmap(pixmap);
            }
        }
        return built;
    }();
    return icon;
}

QString applicationIconPath(int size) {
    return QStringLiteral(":/icons/xpcog-%1.png").arg(size);
}

QList<int> applicationIconSizes() {
    QList<int> sizes;
    sizes.reserve(static_cast<qsizetype>(std::size(kSizes)));
    for (const int size : kSizes) {
        sizes.append(size);
    }
    return sizes;
}

}  // namespace xpcog::app
