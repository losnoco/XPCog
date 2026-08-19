#include "AboutDialog.hpp"

#include "xpcog/core/Version.hpp"

#include <QDialogButtonBox>
#include <QFont>
#include <QLabel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <array>

namespace xpcog::app {
namespace {

struct Component {
    const char* name;
    const char* licence;
    const char* purpose;
};

/// What is actually linked in. Kept here rather than generated, because a
/// licence list has to be right rather than convenient -- a library dropped
/// from the build should be removed deliberately, not vanish silently.
constexpr std::array kComponents = {
    Component{"Qt 6", "LGPL-3.0", "user interface"},
    Component{"FLAC", "BSD-3-Clause", "FLAC decoding"},
    Component{"libvorbis / libogg", "BSD-3-Clause", "Ogg Vorbis decoding"},
    Component{"Opus / opusfile", "BSD-3-Clause", "Opus decoding"},
    Component{"minimp3", "CC0-1.0", "MP3 decoding"},
    Component{"WavPack", "BSD-3-Clause", "WavPack decoding"},
    Component{"FFmpeg", "LGPL-2.1", "AAC, ALAC, WMA and more"},
    Component{"TagLib", "LGPL-2.1 / MPL-1.1", "tag reading"},
    Component{"libsoxr", "LGPL-2.1", "sample-rate conversion"},
    Component{"SQLite", "public domain", "library database"},
    Component{"miniaudio", "public domain / MIT-0", "audio output"},
    Component{"hdcd_decode2", "BSD-2-Clause", "HDCD decoding"},
};

[[nodiscard]] QString buildInfo() {
#if defined(__clang__)
    const QString compiler = QStringLiteral("Clang %1.%2").arg(__clang_major__).arg(__clang_minor__);
#elif defined(_MSC_VER)
    const QString compiler = QStringLiteral("MSVC %1").arg(_MSC_VER);
#elif defined(__GNUC__)
    const QString compiler = QStringLiteral("GCC %1.%2").arg(__GNUC__).arg(__GNUC_MINOR__);
#else
    const QString compiler = QStringLiteral("unknown compiler");
#endif
    return QStringLiteral("%1 · Qt %2").arg(compiler, QStringLiteral(QT_VERSION_STR));
}

}  // namespace

AboutDialog::AboutDialog(const PluginRegistry& registry, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("About XPCog"));
    resize(560, 460);

    auto* title = new QLabel(QStringLiteral("XPCog"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 10);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto* version = new QLabel(
        tr("Version %1 · %2")
            .arg(QString::fromUtf8(kVersionString.data(),
                                   static_cast<qsizetype>(kVersionString.size())),
                 buildInfo()),
        this);
    version->setEnabled(false);

    auto* tabs = new QTabWidget(this);

    // --- About ---
    auto* about = new QTextBrowser;
    about->setOpenExternalLinks(true);
    about->setHtml(
        tr("<p>A cross-platform port of <b>Cog</b>, the macOS audio player, to Qt.</p>"
           "<p>Copyright © 2026 the XPCog authors.<br>"
           "Copyright © 2005–2026 Vincent Spader, Christopher Snowhill and the Cog "
           "authors.</p>"
           "<p>XPCog is free software, licensed under the "
           "<b>GNU General Public License, version 2 or later</b>. It comes with "
           "absolutely no warranty.</p>"
           "<p>Upstream Cog: <a href=\"https://cog.losno.co/\">cog.losno.co</a><br>"
           "Source: <a href=\"https://github.com/losnoco/XPCog\">github.com/losnoco/XPCog</a></p>"));
    tabs->addTab(about, tr("About"));

    // --- Formats ---
    auto* formats = new QTextBrowser;
    QString list = tr("<p>%n decoder(s) compiled in, claiming these extensions:</p>",
                      nullptr, static_cast<int>(registry.decoderCount()));
    list += QStringLiteral("<p style='font-family:monospace'>");
    bool first = true;
    for (const std::string& extension : registry.allExtensions()) {
        if (!first) {
            list += QStringLiteral(" · ");
        }
        list += QString::fromStdString(extension);
        first = false;
    }
    list += QStringLiteral("</p>");
    formats->setHtml(list);
    tabs->addTab(formats, tr("Formats"));

    // --- Licences ---
    auto* licences = new QTextBrowser;
    QString table = QStringLiteral("<table cellpadding='4'>");
    for (const Component& component : kComponents) {
        table += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td><td>%3</td></tr>")
                     .arg(QString::fromLatin1(component.name),
                          QString::fromLatin1(component.licence),
                          tr(component.purpose));
    }
    table += QStringLiteral("</table>");
    licences->setHtml(
        tr("<p>XPCog is built from the following third-party components. Which "
           "codec libraries a given build actually contains depends on how it "
           "was configured — the Formats tab lists what <i>this</i> build can "
           "play.</p>") +
        table);
    tabs->addTab(licences, tr("Licences"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addWidget(version);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);
}

}  // namespace xpcog::app
