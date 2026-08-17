#pragma once

#include <QMainWindow>

namespace xpcog::app {

/// The M0 shell. M3 grows this into the real player window: a QSplitter holding the
/// file tree and playlist, a transport toolbar, and an InfoInspector dock --
/// replacing the 3,181 lines of Cog's Base.lproj/MainMenu.xib.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildPlaceholderUi();
};

}  // namespace xpcog::app
