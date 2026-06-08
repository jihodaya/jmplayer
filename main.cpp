#include <QApplication>
#include <QLoggingCategory>
#include <QDebug>
#include <iostream>
#include <QLocalSocket>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // Set DPI scaling policy for consistent behavior across all resolutions
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // Do NOT set application/organization names to prevent registry usage
    // app.setApplicationName("MIDI File Player");
    app.setApplicationVersion("1.0");
    // app.setOrganizationName("MIDI Player");

    // Single Instance & IPC Logic
    const QString serverName = "JMPlayer_IPC_Server";
    QLocalSocket socket;
    socket.connectToServer(serverName);

    if (socket.waitForConnected(500)) {
        // Server (another instance) is running
        QStringList args = QApplication::arguments();
        if (args.size() > 1) {
            // Send the file path(s) to the existing instance
            QString filePath = args.at(1);
            QByteArray block = filePath.toUtf8();
            socket.write(block);
            socket.waitForBytesWritten(1000);
        }
        socket.disconnectFromServer();
        return 0; // Terminate this instance
    }

    MainWindow window;
    window.show();

    return app.exec();
}