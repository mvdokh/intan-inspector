#include "main_window.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Spike Sort Viewer"));

    MainWindow window;
    window.show();

    const QStringList arguments = app.arguments();
    if (arguments.size() > 1) {
        window.loadPath(arguments[1]);
    } else {
        const QString defaultInfo = QDir::current().absoluteFilePath(QStringLiteral("info.rhd"));
        if (QFileInfo::exists(defaultInfo)) {
            window.loadPath(defaultInfo);
        }
    }

    return app.exec();
}
