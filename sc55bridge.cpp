//
// sc55bridge.cpp — see sc55bridge.h for why this exists.
//
#include "sc55bridge.h"

#include <QRegularExpression>
#include "uistrings.h"
#include "settingsmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProcess>
#include <QThread>
#include <QDebug>
#include <QApplication>
#include <QWidget>
#include <QRect>
#include <QProgressDialog>
#include <QEventLoop>
#include <algorithm>
#include <chrono>

const char* Sc55Bridge::DeviceLabel()
{
    return "[Nuked SC-55]";
}

QString Sc55Bridge::InstallDir()
{
    return QCoreApplication::applicationDirPath() + "/NukedSC55";
}

QString Sc55Bridge::ExecutablePath()
{
    const QString path = InstallDir() + "/nuked-sc55.exe";
    return QFileInfo::exists(path) ? path : QString();
}

bool Sc55Bridge::IsInstalled()
{
    return !ExecutablePath().isEmpty();
}

void Sc55Bridge::EnsureInstallDir()
{
    QDir dir(InstallDir());
    if (!dir.exists())
        QDir().mkpath(InstallDir());

    // The note itself is placed by the build scripts, from
    // sc55/NukedSC55_README.txt (and the _ENG variant). It used to be written
    // out here as well; two copies of the same text is how it goes stale, and
    // this one had already outlived two of its own claims - that mk1 ROMs
    // cannot be used, and that a stock emulator build will do (2026-07-29).
}

QString Sc55Bridge::UnavailableReason()
{
    if (ExecutablePath().isEmpty()) {
        // Deliberately leads with the patch requirement. Pointing straight at
        // the download used to be the whole message, but a stock build does not
        // open the channel jmp connects on and simply stays silent - so that
        // advice sent people down a dead end (2026-07-29).
        return LSTR(
            u8"Nuked-SC55이 설치되어 있지 않습니다.\n\n"
            u8"아래 폴더에 넣으면 loopMIDI 없이 사용할 수 있습니다:\n%1\n\n"
            u8"단, 배포되는 원본 빌드로는 소리가 나지 않습니다.\n"
            u8"함께 들어있는 패치를 적용해 직접 빌드해야 합니다:\n%2\n\n"
            u8"1. 위 폴더의 안내문(README.txt)과 패치 설명을 읽기\n"
            u8"2. 패치를 적용해 빌드한 nuked-sc55.exe 를 폴더 바로 아래에 넣기\n"
            u8"3. ROM 파일을 같은 폴더에 넣기 (어떤 세트든 자동 판별됩니다)\n\n"
            u8"라이선스와 저작권 문제로 프로그램과 ROM은 함께 배포하지 않습니다.",

            u8"Nuked-SC55 is not installed.\n\n"
            u8"Put a copy in this folder to use it without loopMIDI:\n%1\n\n"
            u8"Note that a stock build stays silent - it must be built with the "
            u8"patch supplied alongside jmp:\n%2\n\n"
            u8"1. Read README.txt in that folder, and the patch notes\n"
            u8"2. Put your patched nuked-sc55.exe directly inside the folder\n"
            u8"3. Add ROM files to the same folder (any set is detected)\n\n"
            u8"The emulator and its ROMs are not shipped with jmp for licence "
            u8"and copyright reasons.")
            .arg(QDir::toNativeSeparators(InstallDir()),
                 QDir::toNativeSeparators(
                     QCoreApplication::applicationDirPath() + "/emulator-patch"));
    }

    // No romset restriction. The patched emulator delivers pipe bytes to the
    // main MCU's MIDI UART and never touches the emulated serial hardware, so
    // any model works - the mk2/st requirement that used to be enforced here
    // belonged to the old serial path (2026-07-29). Only a completely empty
    // folder is worth complaining about.
    const QDir dir(InstallDir());
    const bool bHasAnyRom = !dir.entryList(QStringList() << "*.bin", QDir::Files).isEmpty();

    if (!bHasAnyRom) {
        return LSTR(
            u8"ROM 파일이 없습니다.\n\n%1 폴더에 ROM을 넣어 주세요.\n\n"
            u8"SC-55, SC-55mk2 등 어떤 세트든 자동으로 판별됩니다.\n"
            u8"(rom1.bin, rom2.bin ... 또는 sc55_rom1.bin, sc55_rom2.bin ...)",

            u8"No ROM files found.\n\nPut a ROM set in %1.\n\n"
            u8"Any model is detected automatically - SC-55, SC-55mk2 and the rest.\n"
            u8"(rom1.bin, rom2.bin ... or sc55_rom1.bin, sc55_rom2.bin ...)")
            .arg(QDir::toNativeSeparators(InstallDir()));
    }

    return QString();
}

Sc55Bridge::Sc55Bridge(QObject* parent)
    : QObject(parent),
      m_pProcess(nullptr),
      m_bRunning(false)
#ifdef _WIN32
    , m_hPipe(INVALID_HANDLE_VALUE)
#endif
{
}

Sc55Bridge::~Sc55Bridge()
{
    Stop();
}

#ifdef _WIN32

// A name unlikely to collide with anything else on the machine.
static const wchar_t* kPipeName = L"\\\\.\\pipe\\jmplayer_sc55";

bool Sc55Bridge::CreatePipe()
{
    ClosePipe();

    m_hPipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,            // one client - the emulator
        64 * 1024,    // out buffer
        64 * 1024,    // in buffer
        0,
        nullptr);

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        m_error = LSTR(u8"MIDI 파이프를 만들지 못했습니다.",
                       u8"Could not create the MIDI pipe.");
        return false;
    }
    return true;
}

void Sc55Bridge::ClosePipe()
{
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_hPipe);
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
}

void Sc55Bridge::Queue(const uint8_t* pData, size_t nSize)
{
    if (!m_bRunning || nSize == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.insert(m_queue.end(), pData, pData + nSize);
    }
    m_queueCv.notify_one();
}

// Drains the queue at the emulated serial rate. Anything faster overruns the
// sub-MCU's unguarded 1024-byte buffer - see SerialBytesPerSecond in the header.
// The painted bounds of a window, which is NOT what GetWindowRect returns:
// since Aero, a resizable window's rectangle includes an invisible border a few
// pixels wide on the left, right and bottom. Centring on those numbers puts the
// window visibly off to one side, because the padding is counted as if it were
// part of the picture. DWM knows the real bounds - ask it, and fall back to the
// plain rectangle if it won't answer (2026-07-29).
static bool VisibleFrame(HWND hwnd, RECT* pOut)
{
    using PFN_DwmGetWindowAttribute = HRESULT (WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static PFN_DwmGetWindowAttribute pGet = [] {
        HMODULE h = LoadLibraryW(L"dwmapi.dll");      // loaded lazily: no import needed
        return h ? (PFN_DwmGetWindowAttribute) GetProcAddress(h, "DwmGetWindowAttribute")
                 : nullptr;
    }();

    constexpr DWORD kExtendedFrameBounds = 9;         // DWMWA_EXTENDED_FRAME_BOUNDS
    if (pGet && SUCCEEDED(pGet(hwnd, kExtendedFrameBounds, pOut, sizeof(RECT))))
        return true;
    return GetWindowRect(hwnd, pOut) != FALSE;
}

// Finds the emulator's top-level window and parks it directly above jmp, so the
// two don't sit on top of each other. The window only exists a moment after the
// process starts, hence the polling.
void Sc55Bridge::PlaceEmulatorWindow()
{
    if (!m_pProcess)
        return;

    struct Ctx { DWORD pid; HWND hwnd; } ctx { (DWORD) m_pProcess->processId(), nullptr };
    if (ctx.pid == 0)
        return;

    for (int i = 0; i < 40 && !ctx.hwnd; ++i) {          // up to ~4 s
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                c->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.hwnd)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ctx.hwnd || m_nRefW <= 0)
        return;

    RECT rWin {}, rVis {};
    if (!GetWindowRect(ctx.hwnd, &rWin))
        return;
    if (!VisibleFrame(ctx.hwnd, &rVis))
        rVis = rWin;

    // Everything below is worked out in painted coordinates, so what lines up on
    // screen is what the user actually sees.
    const int w = rVis.right - rVis.left;
    const int h = rVis.bottom - rVis.top;

    // SetWindowPos moves the window RECTANGLE, not the painted area, so the
    // difference between the two has to come back off at the end.
    const int dx = rVis.left - rWin.left;
    const int dy = rVis.top - rWin.top;

    // Sit just above jmp, centred on it. If there isn't room above (jmp near the
    // top of the screen), drop it underneath instead of shoving it off-screen.
    const int nGap = 8;
    int x = m_nRefX + (m_nRefW - w) / 2;
    int y = m_nRefY - h - nGap;
    if (y < 0)
        y = m_nRefY + m_nRefH + nGap;

    // Keep it on the monitor jmp is on - centring can push it past an edge when
    // jmp sits near one, or when the emulator window is wider than jmp.
    HMONITOR hMon = MonitorFromPoint(POINT { m_nRefX + m_nRefW / 2, m_nRefY + m_nRefH / 2 },
                                     MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        const RECT& wa = mi.rcWork;
        if (x < wa.left)            x = wa.left;
        if (x + w > wa.right)       x = wa.right - w;
        if (y < wa.top)             y = wa.top;
        if (y + h > wa.bottom)      y = wa.bottom - h;
    }

    SetWindowPos(ctx.hwnd, nullptr, x - dx, y - dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Sc55Bridge::SenderLoop()
{
    using clock = std::chrono::steady_clock;

    // Park the emulator's window out of jmp's way, then let the firmware finish
    // booting before anything is sent. Both happen here rather than in Start()
    // so selecting the device doesn't freeze the UI.
    PlaceEmulatorWindow();

    {
        const auto until = clock::now() + std::chrono::milliseconds(BootSettleMs);
        while (m_bSenderRun.load(std::memory_order_acquire) && clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Releases Start(), which is holding the UI until this point so no song can
    // begin while the firmware is still coming up.
    m_bReady.store(true, std::memory_order_release);

    // One chunk every 2 ms keeps the pacing smooth without waking constantly.
    // At low rates that is a fraction of a byte per tick, so carry the
    // remainder rather than rounding it away - rounding 2.0 down to 2 would
    // silently change the rate.
    constexpr int nTickMs = 2;
    const double nPerTickExact = (double) m_nSerialBytesPerSecond * nTickMs / 1000.0;
    double credit = 0.0;
    auto next = clock::now();

    std::vector<uint8_t> chunk;
    chunk.reserve(64);

    while (m_bSenderRun.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);

            // At the ceiling the limiter is off, so send the moment something
            // arrives instead of gathering it into 2 ms clumps. Waiting for the
            // next tick put every event up to 2 ms late, which is audible as
            // dragging in fast passages - the emulated MIDI cable delivers a
            // byte in ~320 us (2026-07-29).
            if (m_nSerialBytesPerSecond >= 31250) {
                if (m_queue.empty()) {
                    m_queueCv.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                chunk.assign(m_queue.begin(), m_queue.end());
                m_queue.clear();
                lock.unlock();

                if (m_hPipe != INVALID_HANDLE_VALUE) {
                    DWORD nWritten = 0;
                    if (!WriteFile(m_hPipe, chunk.data(), (DWORD) chunk.size(), &nWritten, nullptr)) {
                        qWarning() << "[Sc55Bridge] pipe write failed - stopping";
                        m_bSenderRun.store(false, std::memory_order_release);
                        QMetaObject::invokeMethod(this, [this]() {
                            Stop();
                            emit emulatorStopped();
                        }, Qt::QueuedConnection);
                        return;
                    }
                }
                continue;
            }

            if (m_queue.empty()) {
                // Nothing pending: sleep until something arrives rather than
                // spinning, and restart the clock so a quiet stretch does not
                // bank up credit and then burst.
                m_queueCv.wait_for(lock, std::chrono::milliseconds(50));
                next = clock::now();
                credit = 0.0;
                continue;
            }
            credit += nPerTickExact;
            const size_t nAllowed = (size_t) credit;
            if (nAllowed == 0) {
                lock.unlock();
                next += std::chrono::milliseconds(nTickMs);
                std::this_thread::sleep_until(next);
                continue;
            }
            const size_t n = std::min<size_t>(nAllowed, m_queue.size());
            credit -= (double) n;
            chunk.assign(m_queue.begin(), m_queue.begin() + n);
            m_queue.erase(m_queue.begin(), m_queue.begin() + n);
        }

        if (m_hPipe != INVALID_HANDLE_VALUE && !chunk.empty()) {
            DWORD nWritten = 0;
            if (!WriteFile(m_hPipe, chunk.data(), (DWORD) chunk.size(), &nWritten, nullptr)) {
                // The emulator went away mid-song. Stop rather than keep writing
                // into a dead handle on every note.
                qWarning() << "[Sc55Bridge] pipe write failed - stopping";
                m_bSenderRun.store(false, std::memory_order_release);
                QMetaObject::invokeMethod(this, [this]() {
                    Stop();
                    emit emulatorStopped();
                }, Qt::QueuedConnection);
                return;
            }
        }

        next += std::chrono::milliseconds(nTickMs);
        std::this_thread::sleep_until(next);
    }
}

bool Sc55Bridge::Start()
{
    if (m_bRunning)
        return true;

    m_error.clear();

    const QString exe = ExecutablePath();
    if (exe.isEmpty()) {
        m_error = UnavailableReason();
        return false;
    }

    // The pipe has to exist before the emulator starts - it is the client.
    if (!CreatePipe())
        return false;

    m_pProcess = new QProcess(this);
    m_pProcess->setWorkingDirectory(InstallDir());
    connect(m_pProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Sc55Bridge::onProcessFinished);

    QStringList args;
    // No "-st": the rear COMPUTER switch stays at MIDI on purpose. In the
    // PC-1/PC-2/Mac positions the firmware reads the serial port and IGNORES
    // MIDI IN, which silences the patched build entirely - it delivers arriving
    // pipe bytes to the main MCU's UART, i.e. MIDI IN (2026-07-29).
    //
    // An UNPATCHED emulator needs "-st RS232C_1" instead, and will not open the
    // pipe without it.
    args << "-sp" << QString::fromWCharArray(kPipeName)
         << "-d"  << QDir::toNativeSeparators(InstallDir())
         // Reset at startup. This is NOT the same thing as jmp's own per-song
         // reset (midireset/), which is off by default and fires later: the
         // SC-55mk2's firmware has a documented bug where some parameters do
         // not initialise properly at power-on, and a GS reset is the fix.
         // Nuked-SC55 defaults to gs for the mk2 romset for exactly this reason
         // and warns when it isn't given. Launching with "none" left the first
         // notes of a song sounding wrong (2026-07-29).
         << "-r"  << "gs";

    // No --romset: the patched emulator detects the model on its own for every
    // dump, so dropping ROMs in the folder is all the user has to do. (An ini
    // key for this was added and then removed - the emulator was fixed instead.)

    // Shrink the panel GUI. Its artwork fixes the window at 1120x233, which
    // crowds jmp on a normal screen. Settable (ini key Sc55/WindowScale) since
    // the comfortable size depends on the display.
    {
        double scale = SettingsManager::instance()
            .value("Sc55/WindowScale", DefaultWindowScale).toDouble();
        if (scale < 0.25) scale = 0.25;
        if (scale > 4.0)  scale = 4.0;
        if (scale != 1.0)
            args << "--lcd-scale" << QString::number(scale, 'f', 3);
    }

    // Nuked-SC55 makes its own sound; jmp's audio device is not involved in
    // this mode, so jmp's buffer setting cannot help it. Its own buffer is
    // small enough that another program loading - a game grabbing the CPU - can
    // starve it into noise. Off by default so the emulator keeps its own
    // default; set Sc55/AudioBuffer (for example "2048:32") to enlarge it.
    //
    // The flag is "-b <size>[:count]". An earlier build of this passed
    // "-ab:<size>:<count>", which this emulator rejects with "Unknown argument"
    // and exits over - and it exits before opening the pipe, which used to hang
    // jmp outright (see the connect loop below). Reported 2026-08-19.
    //
    // The value is checked here rather than passed through: a typo in a hand
    // edited settings.ini must not be able to stop the emulator from starting.
    // Written [0-9] rather than \d on purpose - as a C++ literal "\d" is an
    // unknown escape that collapses to "d", which compiles with only a warning
    // and then rejects every valid value.
    {
        const QString ab = SettingsManager::instance()
            .value("Sc55/AudioBuffer", QString()).toString().trimmed();
        if (!ab.isEmpty()) {
            static const QRegularExpression re(QStringLiteral("^[0-9]{1,6}(:[0-9]{1,4})?$"));
            if (re.match(ab).hasMatch()) {
                args << "-b" << ab;
            } else {
                qWarning() << "[SC55] ignoring Sc55/AudioBuffer =" << ab
                           << "- expected <size> or <size>:<count>, e.g. 2048:32";
            }
        }
    }

    m_pProcess->start(exe, args);
    if (!m_pProcess->waitForStarted(5000)) {
        m_error = LSTR(u8"Nuked-SC55을 실행하지 못했습니다.",
                       u8"Could not launch Nuked-SC55.");
        ClosePipe();
        delete m_pProcess;
        m_pProcess = nullptr;
        return false;
    }

    // Wait for it to attach. ConnectNamedPipe blocks until a client opens the
    // pipe; if the emulator dies first this would hang, so poll instead.
    //
    // Two things have to happen on every pass or an emulator that refuses to
    // start takes jmp down with it (2026-07-29, reported as "jmp hangs when I
    // use a different ROM set"):
    //
    //  * Keep the event loop turning. Sleeping outright left the UI frozen for
    //    the full timeout, which Windows reports as "not responding".
    //  * Drain the child's output. Its stdout/stderr are pipes; if nobody reads
    //    them the buffer fills and the emulator BLOCKS mid-startup - so a ROM
    //    problem that produces a few screens of complaint never even reaches
    //    the point of opening our pipe, and the wait always ran to the timeout.
    // ...and the poll only actually polls if the handle is non-blocking. The
    // pipe is created PIPE_WAIT, so ConnectNamedPipe parks the calling thread
    // until a client shows up: the loop below would never reach its second
    // iteration, never drain the child, never notice it had died, and never
    // time out. That is why a bad command-line argument - the emulator rejects
    // it and exits before opening the pipe - froze jmp until it was killed
    // (2026-08-19). Switch to PIPE_NOWAIT for the wait, so ConnectNamedPipe
    // returns ERROR_PIPE_LISTENING straight away, then put it back afterwards
    // because every read and write after this point expects to block.
    {
        DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT;
        SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr);
    }

    const int nStep = 100;
    int nWaited = 0;
    bool bConnected = false;
    QString childOutput;
    while (nWaited < ConnectTimeoutMs) {
        if (ConnectNamedPipe(m_hPipe, nullptr)) {
            bConnected = true;
            break;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            bConnected = true;
            break;
        }
        // ERROR_PIPE_LISTENING is the non-blocking "nobody yet"; anything else
        // is a real failure and waiting out the timeout would not help.
        if (err != ERROR_PIPE_LISTENING && err != ERROR_NO_DATA) {
            qWarning() << "[SC55] ConnectNamedPipe failed, error" << err;
            break;
        }
        childOutput += QString::fromLocal8Bit(m_pProcess->readAllStandardError());
        childOutput += QString::fromLocal8Bit(m_pProcess->readAllStandardOutput());
        if (m_pProcess->state() != QProcess::Running)
            break;                      // emulator exited - usually a ROM problem
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, nStep);
        QThread::msleep(20);
        nWaited += nStep;
    }

    // Back to blocking for the rest of the session.
    {
        DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
        SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr);
    }

    if (!bConnected) {
        childOutput += QString::fromLocal8Bit(m_pProcess->readAllStandardError());
        childOutput += QString::fromLocal8Bit(m_pProcess->readAllStandardOutput());
        m_error = LSTR(u8"Nuked-SC55이 연결되지 않았습니다.\n\n"
                       u8"ROM 파일이 맞지 않을 때 흔히 나타납니다. 아래에 "
                       u8"에뮬레이터가 알려준 내용이 있으면 그것이 원인입니다.",

                       u8"Nuked-SC55 did not connect.\n\n"
                       u8"A mismatched ROM set is the usual cause. If the "
                       u8"emulator reported anything, it appears below.");
        const QString detail = childOutput.trimmed();
        if (!detail.isEmpty())
            m_error += "\n\n" + detail;
        Stop();
        return false;
    }

    // Sample jmp's own frame now, on the UI thread, for the window placement the
    // sender thread will do.
    //
    // Match the MAIN window by class name, not "first visible titled window":
    // ChannelMonitor and LyricsWindow are QMainWindows too and are top-level
    // when floating, so any looser test can centre the emulator on a side panel
    // instead (2026-07-29).
    //
    // Read the frame through the native handle rather than frameGeometry():
    // Qt reports logical pixels, SetWindowPos wants physical ones, and on a
    // scaled display the two disagree.
    m_nRefX = m_nRefY = m_nRefW = m_nRefH = 0;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isWindow() || !w->isVisible())
            continue;
        if (qstrcmp(w->metaObject()->className(), "MainWindow") != 0)
            continue;

        RECT fr {};
        if (VisibleFrame((HWND) w->winId(), &fr)) {
            m_nRefX = fr.left;  m_nRefY = fr.top;
            m_nRefW = fr.right - fr.left;
            m_nRefH = fr.bottom - fr.top;
        } else {
            const QRect g = w->frameGeometry();
            m_nRefX = g.x(); m_nRefY = g.y();
            m_nRefW = g.width(); m_nRefH = g.height();
        }
        break;
    }

    m_bRunning = true;

    // Tunable without a rebuild: the emulator's effective serial rate is not a
    // documented number (see the header), so leave it adjustable while it is
    // being dialled in.
    m_nSerialBytesPerSecond = SettingsManager::instance()
        .value("Sc55/SerialBytesPerSecond", DefaultSerialBytesPerSecond).toInt();
    if (m_nSerialBytesPerSecond < 100)   m_nSerialBytesPerSecond = 100;
    if (m_nSerialBytesPerSecond > 31250) m_nSerialBytesPerSecond = 31250;
    qDebug() << "[Sc55Bridge] pacing at" << m_nSerialBytesPerSecond << "bytes/s";

    // Pace outgoing bytes only once the emulator is really listening.
    m_bReady.store(false, std::memory_order_release);
    m_bSenderRun.store(true, std::memory_order_release);
    m_sender = std::thread(&Sc55Bridge::SenderLoop, this);

    // Do not return until the firmware has finished booting.
    //
    // The wait used to happen purely on the sender thread, which left jmp free
    // to start a song straight away: several seconds of reset SysEx, patch
    // changes and notes piled up in the queue and were then delivered in one
    // burst when the wait expired. That is why the opening of a song came out
    // with the wrong pitch and tempo, and why the SC-55's title banner never
    // appeared - its animation arrived on top of the music (2026-07-29).
    //
    // A modal dialog with user input excluded keeps the window painting while
    // making it impossible to press play early. This only happens when the
    // device is selected, never during playback.
    {
        // Parented to jmp's main window so it comes up centred on it rather than
        // wherever the screen's middle happens to be. m_nRef* was sampled just
        // above and is in physical pixels; the dialog wants the same space.
        QWidget* pOwner = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isWindow() && w->isVisible()
                && qstrcmp(w->metaObject()->className(), "MainWindow") == 0) {
                pOwner = w;
                break;
            }
        }

        QProgressDialog dlg(LSTR(u8"Nuked-SC55을 준비하는 중입니다...",
                                 u8"Starting Nuked-SC55..."),
                            QString(), 0, 0, pOwner);
        dlg.setWindowTitle("Nuked SC-55");
        dlg.setWindowModality(Qt::ApplicationModal);
        dlg.setCancelButton(nullptr);
        dlg.setMinimumDuration(0);

        // Styled explicitly: jmp themes its widgets one by one rather than
        // setting an application palette, so a plain dialog comes up with dark
        // text on a dark background and reads as blank. Colours match the rest
        // of the UI (mainwindow.cpp).
        dlg.setStyleSheet(
            "QProgressDialog { background-color: #2b2b2b; }"
            "QLabel { color: #ffffff; background: transparent;"
            "         font-size: 13px; padding: 12px 8px; }"
            "QProgressBar { background-color: #1e1e1e; border: 1px solid #555555;"
            "               border-radius: 3px; height: 14px; margin: 0px 8px 8px 8px; }"
            "QProgressBar::chunk { background-color: #0078d4; }");
        dlg.show();

        if (m_nRefW > 0) {
            dlg.adjustSize();
            RECT dr {}, dv {};
            if (GetWindowRect((HWND) dlg.winId(), &dr)) {
                if (!VisibleFrame((HWND) dlg.winId(), &dv))
                    dv = dr;
                const int w = dv.right - dv.left, h = dv.bottom - dv.top;
                SetWindowPos((HWND) dlg.winId(), nullptr,
                             m_nRefX + (m_nRefW - w) / 2 - (dv.left - dr.left),
                             m_nRefY + (m_nRefH - h) / 2 - (dv.top - dr.top),
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        while (!m_bReady.load(std::memory_order_acquire)
               && m_bSenderRun.load(std::memory_order_acquire)) {
            // Same reason as the connect loop: an undrained pipe would stall the
            // emulator part-way through booting. FE_PrintControls alone writes a
            // whole table at startup.
            m_pProcess->readAllStandardError();
            m_pProcess->readAllStandardOutput();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
            QThread::msleep(20);
        }
    }

    return true;
}

void Sc55Bridge::Stop()
{
    m_bRunning = false;

    if (m_sender.joinable()) {
        m_bSenderRun.store(false, std::memory_order_release);
        m_queueCv.notify_all();
        m_sender.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }

    if (m_pProcess) {
        disconnect(m_pProcess, nullptr, this, nullptr);
        if (m_pProcess->state() != QProcess::NotRunning) {
            m_pProcess->terminate();
            if (!m_pProcess->waitForFinished(2000))
                m_pProcess->kill();
        }
        m_pProcess->deleteLater();
        m_pProcess = nullptr;
    }

    ClosePipe();
}

#else // !_WIN32 - jmp is Windows-only, but keep this compiling elsewhere.

bool Sc55Bridge::CreatePipe()                     { return false; }
void Sc55Bridge::ClosePipe()                      {}
void Sc55Bridge::Queue(const uint8_t*, size_t)    {}
void Sc55Bridge::PlaceEmulatorWindow()            {}
void Sc55Bridge::SenderLoop()                     {}
bool Sc55Bridge::Start()                          { m_error = "Windows only."; return false; }
void Sc55Bridge::Stop()                           { m_bRunning = false; }

#endif

void Sc55Bridge::onProcessFinished()
{
    if (!m_bRunning)
        return;                 // our own Stop() - not a surprise
    qWarning() << "[Sc55Bridge] emulator exited on its own";
    Stop();
    emit emulatorStopped();
}

void Sc55Bridge::SendShort(uint8_t nStatus, uint8_t nData1, uint8_t nData2)
{
    // Length is implied by the status byte: program change and channel
    // aftertouch carry one data byte, everything else in this range carries two.
    const uint8_t nHigh = nStatus & 0xF0;
    const size_t nLen = (nHigh == 0xC0 || nHigh == 0xD0) ? 2 : 3;
    const uint8_t buf[3] = { nStatus, nData1, nData2 };
    Queue(buf, nLen);
}

void Sc55Bridge::SendBytes(const std::vector<uint8_t>& data)
{
    if (!data.empty())
        Queue(data.data(), data.size());
}
