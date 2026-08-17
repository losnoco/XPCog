#include "MainWindow.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"

#include <QLabel>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace xpcog::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("XPCog"));
    resize(1024, 640);
    buildPlaceholderUi();
}

void MainWindow::buildPlaceholderUi() {
    PluginRegistry registry;
    registerAllCodecs(registry);

    auto* central = new QWidget(this);
    auto* layout  = new QVBoxLayout(central);
    layout->setAlignment(Qt::AlignCenter);

    const auto banner = versionBanner();
    auto* title = new QLabel(QString::fromUtf8(banner.data(),
                                               static_cast<qsizetype>(banner.size())),
                             central);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 8);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(
        tr("%n codec(s) compiled in", nullptr, static_cast<int>(registry.decoderCount())),
        central);
    subtitle->setAlignment(Qt::AlignCenter);

    layout->addWidget(title);
    layout->addWidget(subtitle);
    setCentralWidget(central);

    statusBar()->showMessage(tr("Milestone 0 — skeleton"));
}

}  // namespace xpcog::app
