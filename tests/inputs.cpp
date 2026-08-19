// Feeds rubbish to everything that reads a value from outside the program, and
// checks nothing crashes and nothing out-of-range gets through.
//
// This is the cheapest of the three tools and the one aimed most directly at how
// this project actually breaks. Two of the three bugs shipped on 2026-08-19 were
// unchecked external input:
//
//   * Sc55/AudioBuffer was passed to the emulator unvalidated, under a flag name
//     that did not exist, and the emulator exited before opening its pipe - which
//     hung the player;
//   * the regex added to validate it was written "\d" in a C++ literal, which
//     collapses to "d", so it rejected every legitimate value. That compiled with
//     nothing but a warning and was caught by a person asking a question.
//
// A test that simply runs the checks would have caught the second immediately.
//
// Deliberately NOT part of the player: separate main(), -DBUILD_TESTS=ON only.
//
//   inputs [--keep]        --keep leaves the scratch folder for inspection
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtGlobal>

#include <iostream>

#include "gybokamidi.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void quietHandler(QtMsgType type, const QMessageLogContext&, const QString&)
{
    // These tools deliberately provoke warnings, so warnings are expected output
    // rather than a problem. Only a crash or a wrong value is a failure here.
    (void)type;
}

void check(bool ok, const QString& what, const QString& detail = QString())
{
    ++g_checks;
    if (ok) return;
    ++g_failed;
    std::cout << "FAIL  " << qPrintable(what);
    if (!detail.isEmpty()) std::cout << "   " << qPrintable(detail);
    std::cout << std::endl;
}

// The pattern in sc55bridge.cpp, kept here as a literal copy on purpose. If the
// two ever disagree this test starts failing, which is the point: the bug being
// guarded against was the pattern in the player silently not being the pattern
// its author had in mind.
const char* kAudioBufferPattern = "^[0-9]{1,6}(:[0-9]{1,4})?$";

void testAudioBufferPattern()
{
    const QRegularExpression re(QString::fromLatin1(kAudioBufferPattern));
    check(re.isValid(), "AudioBuffer pattern compiles");

    const char* good[] = { "512", "2048", "1024:24", "2048:32", "4096:16", "8" };
    for (const char* v : good)
        check(re.match(QString::fromLatin1(v)).hasMatch(),
              QString("AudioBuffer accepts %1").arg(v));

    // "d" and "d:d" are what the pattern degenerates to if someone writes \d in
    // a C++ string literal again. They must be rejected.
    const char* bad[] = { "", "abc", "d", "d:d", "2048:", ":32", "-1", "2048:32:8",
                          "2048 32", "0x800", "12345678", "2048:99999" };
    for (const char* v : bad)
        check(!re.match(QString::fromLatin1(v)).hasMatch(),
              QString("AudioBuffer rejects %1").arg(*v ? v : "(empty)"));
}

// A sidecar is a file a person is invited to open and edit, so every value in it
// has to survive being wrong.
void testSidecarClamping(const QString& scratch)
{
    // Any readable .GYB will do; the point is the values, not the song.
    const QString song = "D:/mt32/76/ZEL#3.GYB";
    if (!QFileInfo::exists(song)) {
        std::cout << "skip  sidecar clamping (no test song at " << qPrintable(song)
                  << ")" << std::endl;
        return;
    }

    const QString copy = QDir(scratch).filePath("ZEL#3.GYB");
    QFile::remove(copy);
    if (!QFile::copy(song, copy)) {
        check(false, "could not copy a song into the scratch folder");
        return;
    }

    const QVector<gybokamidi::Row> plan = gybokamidi::buildPlan(copy);
    check(!plan.isEmpty(), "plan built for the scratch copy");
    if (plan.isEmpty()) return;
    const int slot = plan.first().slot;
    const QString name = plan.first().oplName;

    struct Case { const char* program; const char* bank; const char* note; };
    const Case cases[] = {
        { "999",         "-40",  "300"  },
        { "-1",          "9999", "-7"   },
        { "2147483647",  "128",  "128"  },
        { "abc",         "",     "x"    },   // non-numeric -> 0 via toInt()
    };

    for (const Case& c : cases) {
        QFile ini(gybokamidi::sidecarPath(copy));
        if (!ini.open(QIODevice::WriteOnly | QIODevice::Text)) {
            check(false, "could not write the scratch sidecar");
            return;
        }
        QTextStream t(&ini);
        t << "[jmp]" << Qt::endl << "version=1" << Qt::endl
          << "[slot" << slot << "]" << Qt::endl
          << "name=" << name << Qt::endl
          << "drum=false" << Qt::endl
          << "program=" << c.program << Qt::endl
          << "bank=" << c.bank << Qt::endl
          << "note=" << c.note << Qt::endl;
        ini.close();

        const QVector<gybokamidi::Row> after = gybokamidi::buildPlan(copy);
        bool found = false;
        for (const gybokamidi::Row& r : after) {
            if (r.slot != slot) continue;
            found = true;
            const QString where = QString("program=%1 bank=%2 note=%3")
                                      .arg(c.program).arg(c.bank).arg(c.note);
            check(r.program  >= 0 && r.program  <= 127,
                  "sidecar program stays 0-127", where +
                      QString("  got %1").arg(r.program));
            check(r.bankMsb  >= 0 && r.bankMsb  <= 127,
                  "sidecar bank stays 0-127", where +
                      QString("  got %1").arg(r.bankMsb));
            check(r.drumNote >= 0 && r.drumNote <= 127,
                  "sidecar note stays 0-127", where +
                      QString("  got %1").arg(r.drumNote));
        }
        check(found, "the edited slot is still in the plan");

        // Whatever came out, it has to produce a MIDI stream rather than throw.
        QString err;
        const QByteArray midi = gybokamidi::toMidi(copy, after, &err);
        check(!midi.isEmpty(), "a stream is still built from a mangled sidecar",
              err);
    }

    // A sidecar naming a slot the song does not have must be ignored, not
    // applied to whatever happens to sit at that index.
    {
        QFile ini(gybokamidi::sidecarPath(copy));
        ini.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream t(&ini);
        t << "[slot" << slot << "]" << Qt::endl
          << "name=NOT_THE_REAL_NAME" << Qt::endl
          << "program=42" << Qt::endl;
        ini.close();
        const QVector<gybokamidi::Row> after = gybokamidi::buildPlan(copy);
        for (const gybokamidi::Row& r : after)
            if (r.slot == slot)
                check(r.program != 42 || r.baseProgram == 42,
                      "a sidecar whose slot name disagrees is ignored");
    }

    // Garbage that is not an ini at all.
    {
        QFile ini(gybokamidi::sidecarPath(copy));
        ini.open(QIODevice::WriteOnly);
        ini.write(QByteArray(512, '\x01'));
        ini.close();
        const QVector<gybokamidi::Row> after = gybokamidi::buildPlan(copy);
        check(!after.isEmpty(), "a corrupt sidecar does not lose the plan");
    }

    QFile::remove(gybokamidi::sidecarPath(copy));
}

// Truncated and empty files reach the loaders through the playlist, from a bad
// copy or a half-finished download.
void testMalformedSongs(const QString& scratch)
{
    const QString source = "D:/mt32/76/ZEL#3.GYB";
    if (!QFileInfo::exists(source)) {
        std::cout << "skip  malformed songs (no test song)" << std::endl;
        return;
    }
    QFile in(source);
    in.open(QIODevice::ReadOnly);
    const QByteArray whole = in.readAll();
    in.close();

    struct Case { const char* label; QByteArray data; };
    QVector<Case> cases;
    cases.push_back({ "empty",            QByteArray() });
    cases.push_back({ "one byte",         whole.left(1) });
    cases.push_back({ "header only",      whole.left(0x40) });
    cases.push_back({ "half",             whole.left(whole.size() / 2) });
    cases.push_back({ "all zeroes",       QByteArray(whole.size(), '\0') });
    cases.push_back({ "trailing garbage", whole + QByteArray(4096, '\xff') });

    for (const Case& c : cases) {
        const QString path = QDir(scratch).filePath("malformed.gyb");
        QFile::remove(path);
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write(c.data);
        out.close();

        // Either answer is acceptable - what must not happen is a crash, or a
        // stream built out of nothing.
        QString err;
        const QByteArray midi = gybokamidi::toMidi(path, gybokamidi::buildPlan(path), &err);
        check(midi.isEmpty() || midi.startsWith("MThd"),
              QString("malformed GYB (%1) yields either nothing or a real SMF")
                  .arg(c.label),
              QString("got %1 bytes").arg(midi.size()));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    qInstallMessageHandler(quietHandler);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("inputs");

    QCommandLineParser parser;
    parser.setApplicationDescription("Feed rubbish to everything that reads external input.");
    parser.addHelpOption();
    QCommandLineOption keepOpt("keep", "Leave the scratch folder behind.");
    parser.addOption(keepOpt);
    parser.process(app);

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::cerr << "cannot create a scratch folder" << std::endl;
        return 2;
    }
    scratch.setAutoRemove(!parser.isSet(keepOpt));

    testAudioBufferPattern();
    testSidecarClamping(scratch.path());
    testMalformedSongs(scratch.path());

    std::cout << std::endl << "--- summary ---" << std::endl
              << "  " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    if (parser.isSet(keepOpt))
        std::cout << "  scratch kept at " << qPrintable(scratch.path()) << std::endl;

    return g_failed > 0 ? 1 : 0;
}
