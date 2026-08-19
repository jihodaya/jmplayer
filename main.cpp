#include <QApplication>
#include <QLoggingCategory>
#include <QDebug>
#include <iostream>
#include <QLocalSocket>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include "mainwindow.h"
#include "settingsmanager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>

// Write where a crash happened to <settings folder>/jmp_crash.log.
//
// A crash in a release build otherwise says nothing at all - the window simply
// disappears - which turns every report into guesswork. This records the fault
// code and the call stack, so a user can send one small text file instead.
static LONG WINAPI writeCrashLog(EXCEPTION_POINTERS* info)
{
    static bool alreadyWriting = false;      // a fault inside here must not loop
    if (alreadyWriting) return EXCEPTION_EXECUTE_HANDLER;
    alreadyWriting = true;

    QFile log(SettingsManager::storageDir() + "/jmp_crash.log");
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&log);
        out << "\n==== " << QDateTime::currentDateTime().toString(Qt::ISODate)
            << "  code 0x"
            << QString::number(info->ExceptionRecord->ExceptionCode, 16)
            << "  at 0x"
            << QString::number((quintptr)info->ExceptionRecord->ExceptionAddress, 16)
            << " ====\n";

        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);

        void* frames[48];
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        char buffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;

        for (USHORT i = 0; i < n; ++i) {
            DWORD64 disp = 0;
            if (SymFromAddr(proc, (DWORD64)frames[i], &disp, sym))
                out << "  " << i << ": " << sym->Name << " +" << (qulonglong)disp << "\n";
            else
                out << "  " << i << ": 0x" << QString::number((quintptr)frames[i], 16) << "\n";
        }
        out.flush();
        log.close();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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
        tempDir.entryList(QStringList() << "nob_*.mid" << "oka_*.mid"
                                        << "opl_*.mid", QDir::Files);
    for (const QString& name : stale)
        tempDir.remove(name);
}

int main(int argc, char *argv[])
{
    // Set DPI scaling policy for consistent behavior across all resolutions
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(writeCrashLog);
#endif

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