#include "windows/MainWindow.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("LoSnoCo"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("losno.co"));
    QCoreApplication::setApplicationName(QStringLiteral("XPCog"));

    xpcog::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
