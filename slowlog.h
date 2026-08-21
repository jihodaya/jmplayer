#ifndef SLOWLOG_H
#define SLOWLOG_H

// Where UI time actually goes, recorded rather than reasoned about.
//
// Added 2026-08-21 after "moving between playlist folders while a song played
// froze the window for a few seconds, then recovered". Nothing on that path
// reads a file, so the honest answer was that nobody knew - and this project
// has lost days before to diagnoses that were argued instead of measured (the
// Nuked-SC55 hunt, four wrong ones in a row).
//
// Three instruments, because each answers a different question:
//
//   JMP_SLOW("name")   times a scope and records it if it ran long. Says which
//                      step was slow, but only for steps somebody wrapped.
//
//   startWatchdog()    the GUI thread promises to tick every 50 ms; a WATCHER
//                      THREAD notices when it does not. It has to be a separate
//                      thread: a timer on the GUI thread cannot fire while that
//                      thread is the one stuck, so it only ever reports the
//                      stall after the fact, with the evidence already gone.
//
//   Application::notify()  records which event is being dispatched right now,
//                      so the watcher can name it *while the stall is still
//                      happening*. That is the whole point of the pairing.
//
// The first run (2026-08-21) recorded six stalls of 257-1132 ms and **not one
// scoped timer**, which cleared the entire playlist-navigation path - and
// `savePlaylistTree` separately: the file is 1.73 MB / 11,528 nodes and a full
// serialise, write and fsync measures under 50 ms. So the cost is somewhere
// nobody had thought to look, which is exactly when guessing gets expensive.
//
// The watcher never suspends the GUI thread. It reads atomics, nothing more.
// Sampling a suspended thread's stack was the obvious alternative and was not
// taken: symbolising needs the heap, contention for the heap is one of the
// things being investigated, and a debugging tool that can deadlock the program
// it is debugging is not worth the information.
//
// OFF unless asked for. A shipped build must not write diagnostic files into
// somebody's Documents folder, so everything here is gated on
//
//     [Debug]
//     SlowLog=true
//
// in settings.ini, read once at startup. Disabled, the watcher thread is never
// created and every entry point is one relaxed atomic load - which is why the
// instrumentation can stay in the shipping build instead of being torn out and
// rewritten the next time somebody reports a freeze.
//
// Enabled, it costs two atomic stores per event dispatched and one timer tick
// per 50 ms, and the log file is only created the first time something is
// actually slow.

#include <QElapsedTimer>
#include <QString>

class QObject;

namespace SlowLog {

// Anything at or over this is worth a line. Well under the ~5 s that makes
// Windows paint "(Not Responding)", so a stall that never got that far still
// shows up.
constexpr int kThresholdMs = 250;

// False in a normal install; see the note above. Cheap enough to call anywhere.
bool isEnabled();

void note(const QString& what, qint64 ms);

// Called by Application::notify() around every event dispatch. `cls` must
// outlive the call - QMetaObject::className() returns a static string, which is
// what makes storing a bare pointer safe.
void enterEvent(const char* cls, int type);
void leaveEvent();

// Starts the GUI-thread heartbeat and the watcher thread that reads it.
// `guiParent` owns the heartbeat timer.
void startWatchdog(QObject* guiParent);
void stopWatchdog();

class Scope
{
public:
    explicit Scope(const char* what) : m_what(what)
    {
        if (isEnabled()) m_timer.start();
    }
    ~Scope()
    {
        if (!m_timer.isValid()) return;          // never started: logging is off
        const qint64 ms = m_timer.elapsed();
        if (ms >= kThresholdMs)
            note(QString::fromLatin1(m_what), ms);
    }

private:
    const char* m_what;
    QElapsedTimer m_timer;
};

} // namespace SlowLog

#define JMP_SLOW_CAT2(a, b) a##b
#define JMP_SLOW_CAT(a, b) JMP_SLOW_CAT2(a, b)
#define JMP_SLOW(what) SlowLog::Scope JMP_SLOW_CAT(jmpSlowScope_, __LINE__)(what)

#endif // SLOWLOG_H
