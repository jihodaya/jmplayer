//
// sc55bridge.h
//
// Drives Nuked-SC55 as a child process without needing a virtual MIDI cable.
//
// Nuked-SC55 can take its MIDI input over the SC-55mk2's emulated *serial*
// port, and on Windows that port may be a named pipe. So instead of routing
// through loopMIDI, jmp creates the pipe itself, launches the emulator pointed
// at it, and writes raw MIDI bytes straight down it. The user installs nothing.
//
// The emulator is the pipe CLIENT - it opens a pipe that already exists - so
// the server must be up before the process starts. (Verified 2026-07-28: the
// emulator reports "Unable to open serial port ... file not found" if it is
// launched first.)
//
// **Nuked-SC55 is not distributed with jmp.** Its licence forbids commercial
// redistribution, and adopting it would end jmp's public-domain status. The
// user drops their own copy into the folder below; jmp only looks for it.
// ROM files are the user's too - they are Roland's.
//
#ifndef SC55BRIDGE_H
#define SC55BRIDGE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

class QProcess;

class Sc55Bridge : public QObject
{
    Q_OBJECT

public:
    // The device name shown in jmp's device list. Recognised by
    // MainWindow::onDeviceChanged and MidiPlayer, so keep it in one place.
    static const char* DeviceLabel();

    // Folder the user is expected to put Nuked-SC55 into: <exe dir>/NukedSC55.
    static QString InstallDir();

    // Path to the emulator executable, or empty if it isn't there.
    static QString ExecutablePath();

    // Create the drop folder and its README if missing. Called at startup so
    // there is always somewhere obvious to put the emulator - the folder lives
    // under release\, which the build scripts wipe, so it cannot simply be
    // shipped once.
    static void EnsureInstallDir();

    // True when the executable exists - i.e. the entry is worth offering.
    static bool IsInstalled();

    // Why the emulator can't be used, in the user's language, or empty when it
    // can. Checks for the executable and for a usable ROM set: serial MIDI only
    // exists on the SC-55mk2 and SC-55st, so an mk1-only folder cannot work
    // however well the rest is set up.
    static QString UnavailableReason();

    explicit Sc55Bridge(QObject* parent = nullptr);
    ~Sc55Bridge() override;

    // Create the pipe, launch the emulator and wait for it to attach. Returns
    // false and fills errorString() on failure. Safe to call when already
    // running - it just returns true.
    bool Start();

    // Stop the emulator and drop the pipe. Safe when not running.
    void Stop();

    bool IsRunning() const { return m_bRunning; }
    QString errorString() const { return m_error; }

    // Send raw MIDI. Short messages and SysEx go down the same pipe; the
    // emulated UART is a byte stream and does not care which is which.
    void SendShort(uint8_t nStatus, uint8_t nData1, uint8_t nData2);
    void SendBytes(const std::vector<uint8_t>& data);

signals:
    // Emitted when the emulator exits on its own (crash, or the user closed its
    // window), so the UI can drop back to another device.
    void emulatorStopped();

private slots:
    void onProcessFinished();

private:
    bool CreatePipe();
    void ClosePipe();
    void Queue(const uint8_t* pData, size_t nSize);
    void SenderLoop();

    // Move the emulator's window so it doesn't sit on top of jmp. Runs on the
    // sender thread (the window only appears a moment after launch, so it has
    // to be waited for), using a rectangle captured on the UI thread.
    void PlaceEmulatorWindow();

    // jmp's own frame, sampled in Start() before returning. Reading widget
    // geometry from the worker thread would not be safe.
    int m_nRefX = 0, m_nRefY = 0, m_nRefW = 0, m_nRefH = 0;

    QProcess* m_pProcess;
    bool m_bRunning;
    QString m_error;

    // Outgoing bytes, drained by m_sender at the emulated serial rate. See
    // SerialBytesPerSecond for why this cannot just be written straight out.
    std::deque<uint8_t> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::thread m_sender;
    std::atomic<bool> m_bSenderRun { false };

    // Set by the sender thread once the boot wait is over. Start() blocks on it
    // so a song cannot begin while the firmware is still booting - see the wait
    // in Start() for what went wrong when it could.
    std::atomic<bool> m_bReady { false };

#ifdef _WIN32
    HANDLE m_hPipe;
#endif

    // The emulated firmware only enables its receiver once it has booted; bytes
    // sent before that are dropped on the floor with no error. Measured on an
    // SC-55mk2 romset: silence when sending immediately, correct playback after
    // waiting (2026-07-28).
    //
    // This wait happens on the SENDER THREAD, not in Start(). Blocking the UI
    // thread for six seconds froze jmp's window every time the device was
    // selected. Anything sent meanwhile simply queues up.
    static constexpr int BootSettleMs = 6000;

    // How long to wait for the emulator to open our pipe before giving up.
    static constexpr int ConnectTimeoutMs = 15000;

    // Pacing exists for the stock emulator, which routes pipe bytes to the
    // sub-MCU's emulated RS-232 receiver: that takes one byte per poll with a
    // fixed inter-byte delay, and its 1024-byte buffer has no overflow check, so
    // a song's opening burst is partly overwritten and comes out as wrong
    // instruments, stuck notes and unsteady tempo. Pacing did not cure that
    // (measured 2026-07-29) - the transport itself was wrong.
    //
    // The patched emulator (see sc55/emulator-patch/) hands those bytes to the
    // MAIN MCU's UART instead, the same entry point loopMIDI reached. That path
    // has an 8192-byte buffer and is drained per instruction, so it needs no
    // pacing - hence the default sits at the ceiling, effectively unlimited.
    //
    // The limiter is kept, and stays settable (ini key
    // Sc55/SerialBytesPerSecond), so an UNPATCHED emulator can still be used by
    // lowering it - around 1000 was the best the serial path managed.
    static constexpr int DefaultSerialBytesPerSecond = 31250;

    int m_nSerialBytesPerSecond = DefaultSerialBytesPerSecond;

    // Panel-GUI window scale passed to the patched emulator's --lcd-scale. The
    // artwork fixes the window at 1120x233; 0.75 keeps it readable while
    // leaving room for jmp. Override with ini key Sc55/WindowScale.
    //
    // This was NOT free before 2026-08-20, and the reason is worth keeping.
    // Any scale other than 1.0 makes SDL filter-resample the panel artwork on
    // every frame, and the emulator did that with `SDL_CreateRenderer(..., 0)` -
    // which may quietly fall back to the software renderer - and with
    // `RENDER_SCALE_QUALITY=BEST`, which on D3D9 means anisotropic filtering.
    // On the reporter's i5-8250U that was enough to make the emulator lose
    // realtime: the music slowed and crackled whenever a browser started.
    //
    // It never showed as CPU time - idle was 58.3 % at 0.75 and 58.1 % at 1.0 -
    // because the cost landed on the integrated GPU, which shares its power
    // budget with the CPU cores. Setting 1.0 was what he first heard fix it.
    //
    // The emulator patch now asks for acceleration explicitly, uses linear
    // filtering, and redraws at 33 fps instead of 66, so the default goes back
    // to 0.75. **That means this default assumes a patched emulator built on or
    // after 2026-08-20**; with an older one, 0.75 can still cost what it did.
    // Set Sc55/WindowScale = 1.0 to take the scaling out of the picture.
    static constexpr double DefaultWindowScale = 0.75;

    // Sc55/HidePanel - run the emulator with --no-lcd, no window at all.
    //
    // The panel is redrawn every 15 ms whether or not anyone is looking at it,
    // and on that same laptop the thread doing it used 4.81 s of CPU in a 29 s
    // window - about 16 % of a core, on a machine that has none to spare. Off by
    // default because it costs the SC-55 display; worth it if the sound still
    // breaks up.
    static constexpr bool DefaultHidePanel = false;

    bool m_bHidePanel = DefaultHidePanel;
};

#endif // SC55BRIDGE_H
