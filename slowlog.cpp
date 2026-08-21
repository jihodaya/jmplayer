#include "slowlog.h"
#include "settingsmanager.h"

#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QMetaEnum>
#include <QObject>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <thread>

namespace SlowLog {
namespace {

// One monotonic clock everything reads, started once.
QElapsedTimer& clockRef()
{
    static QElapsedTimer c = [] { QElapsedTimer t; t.start(); return t; }();
    return c;
}

std::atomic<qint64> g_heartbeat{0};        // last time the GUI thread ran
std::atomic<const char*> g_eventClass{nullptr};
std::atomic<int> g_eventType{0};
std::atomic<qint64> g_eventStart{0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_enabled{false};
std::thread g_watcher;

QString eventName(int type)
{
    static const QMetaEnum meta = QMetaEnum::fromType<QEvent::Type>();
    if (const char* key = meta.valueToKey(type))
        return QString::fromLatin1(key);
    return QStringLiteral("type %1").arg(type);
}

void watch()
{
    bool reported = false;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const qint64 now = clockRef().elapsed();
        const qint64 beat = g_heartbeat.load(std::memory_order_acquire);
        const qint64 gap = now - beat;

        if (gap < kThresholdMs) {
            reported = false;
            continue;
        }
        if (reported)
            continue;       // one line per episode, not one per 50 ms
        reported = true;

        // Read what the GUI thread is in the middle of. Order matters: the
        // class pointer is cleared last on the way out, so a non-null read here
        // means the other two belong with it closely enough for a log line.
        const char* cls = g_eventClass.load(std::memory_order_acquire);
        const int type = g_eventType.load(std::memory_order_relaxed);
        const qint64 started = g_eventStart.load(std::memory_order_relaxed);

        if (cls) {
            note(QStringLiteral("STUCK in %1 / %2 (%3 ms into it)")
                     .arg(QString::fromLatin1(cls), eventName(type))
                     .arg(now - started),
                 gap);
        } else {
            // No event in flight: the loop is blocked somewhere that is not an
            // event handler at all - the platform message pump, a nested modal
            // loop, a wait inside Qt, or the thread simply not being scheduled.
            note(QStringLiteral("STUCK outside event dispatch"), gap);
        }
    }
}

} // namespace

bool isEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void note(const QString& what, qint64 ms)
{
    if (!isEnabled()) return;

    // Opened per line on purpose. These are rare by construction, and a handle
    // held open would keep the file locked against the user reading it while
    // the program runs - which is exactly when they want to look.
    QFile f(SettingsManager::storageDir() + "/jmp_slow.log");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream(&f) << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                    << "  " << qSetFieldWidth(6) << ms << qSetFieldWidth(0)
                    << " ms  " << what << '\n';
}

void enterEvent(const char* cls, int type)
{
    if (!isEnabled()) return;
    g_eventStart.store(clockRef().elapsed(), std::memory_order_relaxed);
    g_eventType.store(type, std::memory_order_relaxed);
    g_eventClass.store(cls, std::memory_order_release);
}

void leaveEvent()
{
    if (!isEnabled()) return;
    g_eventClass.store(nullptr, std::memory_order_release);
}

void startWatchdog(QObject* guiParent)
{
    // Off unless settings.ini says otherwise. Read once: the watcher thread
    // must never touch QSettings, and a flag that could change under it would
    // be a race for no benefit.
    if (!SettingsManager::instance().value("Debug/SlowLog", false).toBool())
        return;
    g_enabled.store(true, std::memory_order_relaxed);

    clockRef();
    g_heartbeat.store(clockRef().elapsed(), std::memory_order_release);

    QTimer* beat = new QTimer(guiParent);
    QObject::connect(beat, &QTimer::timeout, guiParent, [] {
        g_heartbeat.store(clockRef().elapsed(), std::memory_order_release);
    });
    beat->start(50);

    g_running.store(true, std::memory_order_relaxed);
    g_watcher = std::thread(watch);
}

void stopWatchdog()
{
    if (!g_running.exchange(false))
        return;
    if (g_watcher.joinable())
        g_watcher.join();
}

} // namespace SlowLog
