#include <QApplication>
#include <QLoggingCategory>
#include <QDebug>
#include <iostream>
#include <QLocalSocket>
#include <QDir>
#include "mainwindow.h"

// Remove leftover extracted-MIDI temp files from a previous run.
//
// NOB/OKM/OKW are MIDI wrapped in a container; playback unwraps the MIDI into
// <exe dir>/temp/{nob,oka}_XXXXXX.mid and hands that to the MIDI engine. Those
// QTemporaryFiles delete themselves on a normal exit, but a crash or a kill
// leaves them behind, and nothing swept them up on the next launch - so they
// piled up over time. Clear them here, once, before the window opens.
//
// Only run after this process has claimed the single-instance server, so it
// never deletes a file another running instance is using. Only our own
// prefixes are touched; the temp folder itself is left in place.
static void cleanLeftoverTempMidi()
{
    QDir tempDir(QCoreApplication::applicationDirPath() + "/temp");
    if (!tempDir.exists())
        return;
    const QStringList stale =
        tempDir.entryList(QStringList() << "nob_*.mid" << "oka_*.mid", QDir::Files);
    for (const QString& name : stale)
        tempDir.remove(name);
}

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

    // This instance is the server (no other was running), so no other process
    // is holding a temp file - safe to sweep leftovers from a past crash.
    cleanLeftoverTempMidi();

    MainWindow window;
    window.show();

    return app.exec();
}