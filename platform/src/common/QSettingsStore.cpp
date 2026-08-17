#include "xpcog/platform/QSettingsStore.hpp"

#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <QVariant>

namespace xpcog::platform {
namespace {

[[nodiscard]] QString toQt(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

QSettingsStore::QSettingsStore() = default;

QSettingsStore::QSettingsStore(const QString& filePath)
    : settings_(filePath, QSettings::IniFormat) {}

std::optional<std::string> QSettingsStore::getRaw(std::string_view key) const {
    const QVariant value = settings_.value(toQt(key));
    if (!value.isValid()) {
        return std::nullopt;
    }
    // Everything crosses this boundary as a string, because that is what the
    // interface promises and what Settings parses. QVariant converts numbers and
    // booleans for us, which is what makes Cog's plist -- where `repeat` is a
    // real integer, not a string -- read without translation.
    return value.toString().toStdString();
}

void QSettingsStore::setRaw(std::string_view key, std::string_view value) {
    settings_.setValue(toQt(key), toQt(value));
}

void QSettingsStore::remove(std::string_view key) { settings_.remove(toQt(key)); }

void QSettingsStore::sync() { settings_.sync(); }

std::string libraryDatabasePath() {
    // AppDataLocation is ~/Library/Application Support/XPCog on macOS,
    // %APPDATA%/XPCog on Windows and ~/.local/share/XPCog on Linux.
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir{}.mkpath(directory);
    return QDir{directory}.filePath(QStringLiteral("library.db")).toStdString();
}

}  // namespace xpcog::platform
