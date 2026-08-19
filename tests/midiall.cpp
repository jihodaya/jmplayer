// Loads MIDI-side songs through MidiPlayer and records what it made of each
// one: length, tick count, the sound-module verdict, and the decoded lyrics.
//
// loadall already opens these files, but for `.mid` it only checks there is an
// MThd header - which is almost nothing, and they are 90 % of the library. This
// covers the parts that actually have logic in them:
//
//   * the SMF parse and the tempo map, which the reported duration is a direct
//     function of;
//   * calculateSoundModeReliability(), the GM / MT-32 / GS / XG badge, whose
//     comments record three heuristics that were measured and rejected and one
//     rewrite that went from 68 % to 100 %. A silent change there would be very
//     hard to notice by ear;
//   * lyric decoding - Johab for `.NOB`, EUC-KR for the rest - which is 208 call
//     sites across the codebase and the reason several of these formats are
//     supported at all.
//
// **Realtime sequencing is deliberately not covered.** What bytes go out and
// when is driven by a wall clock in a playback thread; running 130,000 songs
// through it would take weeks, and faking the clock means a seam in the player
// that exists only for the test. The offline half - parse, tempo, detection,
// lyrics - is what is checked here.
//
// Nothing in the player was changed to make this possible: the verdict arrives
// on the soundModeDetected / soundModeReliabilityChanged signals, and everything
// else is already public.
//
// Deliberately NOT part of the player: separate main(), -DBUILD_TESTS=ON only.
//
//   midiall <folder> [--formats mid,nob,okm] [--stride N] [--limit N]
//           [--baseline FILE] [--quiet]
//
// --stride 20 takes every twentieth file, which is how to sample 116,000 `.mid`
// in a minute instead of half an hour. The stride is part of the baseline's
// first line, so comparing runs made with different strides is caught.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QTextStream>
#include <QtGlobal>

#include <iostream>
#include <map>

#include "midiplayer.h"

namespace {

void quietHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (type == QtDebugMsg || type == QtInfoMsg) return;
    std::cerr << msg.toLocal8Bit().constData() << std::endl;
}

const char* modeName(int mode)
{
    switch (mode) {
        case 0:  return "GM";
        case 1:  return "MT-32";
        case 2:  return "GS";
        case 3:  return "XG";
        default: return "unknown";
    }
}

struct Result {
    bool ok = false;
    QString detail;
};

// One player per file. They are cheap next to parsing, and reusing one would
// make each result depend on what was loaded before it - which is exactly the
// kind of order dependence that makes a baseline useless.
Result inspect(const QString& path)
{
    Result r;
    MidiPlayer player;

    int mode = -1;
    int confidence = -1;
    QString method;
    bool strong = false;

    QObject::connect(&player, &MidiPlayer::soundModeDetected,
                     [&](int m) { mode = m; });
    QObject::connect(&player, &MidiPlayer::soundModeReliabilityChanged,
                     [&](const SoundModeReliability& rel) {
                         confidence = rel.confidenceScore;
                         method = rel.detectionMethod;
                         strong = rel.hasStrongEvidence;
                     });

    if (path.endsWith(".nob", Qt::CaseInsensitive))
        player.setIsNobFile(true);

    if (!player.loadMidiFile(path)) { r.detail = "load failed"; return r; }

    // Duration is the one number set during parsing, so it is readable straight
    // after a load. getTotalTicks() is not: it reads a global event list that
    // createGlobalEventList() builds when playback starts, and asking for it
    // here returns 0 for every file - which is what the first run of this tool
    // reported for all 7,884 of them. Duration is a direct function of the
    // tempo map anyway, so nothing is lost by leaving ticks out.
    const unsigned long ms = player.getTotalDuration();

    // A song that parses but reports no length will not play, and neither
    // loadMidiFile nor the UI says so.
    if (ms == 0) { r.detail = "0 ms"; return r; }

    // Lyrics go in as a hash rather than as text: the point is to notice when
    // the decoding changes, and the text itself is period commercial material
    // that has no business in a file that might be read by anyone.
    const QStringList lyrics = player.extractLyrics();
    const QList<unsigned long> sylTicks = player.extractLyricSyllableTicks();

    QCryptographicHash h(QCryptographicHash::Md5);
    for (const QString& line : lyrics) h.addData(line.toUtf8());
    for (unsigned long t : sylTicks)
        h.addData(QByteArrayView(reinterpret_cast<const char*>(&t), sizeof(t)));
    const QString lyricHash = lyrics.isEmpty() && sylTicks.isEmpty()
                                  ? QStringLiteral("-")
                                  : QString::fromLatin1(h.result().toHex().left(12));

    r.ok = true;
    r.detail = QString("ms=%1 mode=%2 conf=%3 lines=%4 syl=%5 lyr=%6")
                   .arg(ms)
                   .arg(modeName(mode))
                   .arg(confidence)
                   .arg(lyrics.size())
                   .arg(sylTicks.size())
                   .arg(lyricHash);
    if (strong) r.detail += " strong";
    if (!method.isEmpty()) r.detail += "  via=" + method;
    return r;
}

}  // namespace

int main(int argc, char** argv)
{
    qInstallMessageHandler(quietHandler);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("midiall");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Load MIDI-side songs through MidiPlayer and record what it parsed.");
    parser.addHelpOption();
    parser.addPositionalArgument("folder", "Folder to walk, recursively.");
    QCommandLineOption formatsOpt("formats",
        "Comma-separated extensions to include.", "list", "mid,midi,nob,okm,okw");
    QCommandLineOption strideOpt("stride",
        "Take every Nth file. 1 is everything.", "n", "1");
    QCommandLineOption limitOpt("limit",
        "Stop after this many files. 0 is no limit.", "n", "0");
    QCommandLineOption baselineOpt("baseline",
        "Compare against this recorded run; written if absent.", "file");
    QCommandLineOption quietOpt("quiet", "Only the summary and what changed.");
    parser.addOption(formatsOpt);
    parser.addOption(strideOpt);
    parser.addOption(limitOpt);
    parser.addOption(baselineOpt);
    parser.addOption(quietOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) parser.showHelp(2);
    const QString root = args.first();
    if (!QFileInfo(root).isDir()) {
        std::cerr << "not a folder: " << qPrintable(root) << std::endl;
        return 2;
    }

    QStringList filters;
    for (const QString& e : parser.value(formatsOpt).split(',', Qt::SkipEmptyParts))
        filters << "*." + e.trimmed().toLower();

    const int stride = qMax(1, parser.value(strideOpt).toInt());
    const int limit = parser.value(limitOpt).toInt();
    const bool quiet = parser.isSet(quietOpt);

    QElapsedTimer clock;
    clock.start();

    std::map<QString, QString> current;
    int seen = 0, ok = 0, failed = 0;

    QDirIterator it(root, filters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if ((seen++ % stride) != 0) continue;
        if (limit > 0 && (ok + failed) >= limit) break;

        const Result r = inspect(path);
        const QString rel = QDir(root).relativeFilePath(path);
        current[rel] = (r.ok ? "ok   " : "FAIL ") + r.detail;
        if (r.ok) ok++;
        else {
            failed++;
            if (!quiet)
                std::cout << "FAIL  " << qPrintable(rel) << "   "
                          << qPrintable(r.detail) << std::endl;
        }
    }

    std::cout << std::endl << "--- summary ---" << std::endl
              << "  " << (ok + failed) << " files inspected (stride " << stride
              << " of " << seen << " found), " << failed << " failed, "
              << clock.elapsed() / 1000.0 << " s" << std::endl;

    const QString baselinePath = parser.value(baselineOpt);
    if (baselinePath.isEmpty()) return failed > 0 ? 1 : 0;

    if (!QFile::exists(baselinePath)) {
        QFile out(baselinePath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            std::cerr << "cannot write baseline" << std::endl;
            return 2;
        }
        QTextStream t(&out);
        t << "# midiall baseline  " << QDateTime::currentDateTime().toString(Qt::ISODate)
          << "  stride " << stride << ", " << (ok + failed) << " files under "
          << root << Qt::endl;
        for (const auto& kv : current) t << kv.first << "\t" << kv.second << Qt::endl;
        std::cout << "baseline written: " << qPrintable(baselinePath) << std::endl;
        return 0;
    }

    std::map<QString, QString> before;
    int baselineStride = -1;
    {
        QFile in(baselinePath);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "cannot read baseline" << std::endl;
            return 2;
        }
        QTextStream t(&in);
        while (!t.atEnd()) {
            const QString line = t.readLine();
            if (line.startsWith('#')) {
                // Comparing a stride-20 run against a stride-1 baseline would
                // report thousands of "missing" files and nothing useful.
                const int at = line.indexOf("stride ");
                if (at > 0) baselineStride = line.mid(at + 7).split(',').first().toInt();
                continue;
            }
            if (line.isEmpty()) continue;
            const int tab = line.indexOf('\t');
            if (tab > 0) before[line.left(tab)] = line.mid(tab + 1);
        }
    }
    if (baselineStride > 0 && baselineStride != stride) {
        std::cerr << "baseline was recorded with stride " << baselineStride
                  << ", this run used " << stride
                  << " - rerun with --stride " << baselineStride << std::endl;
        return 2;
    }

    int changed = 0, fixed = 0, added = 0, missing = 0;
    for (const auto& kv : current) {
        auto old = before.find(kv.first);
        if (old == before.end()) { added++; continue; }
        if (old->second == kv.second) continue;
        const bool wasOk = old->second.startsWith("ok");
        const bool nowOk = kv.second.startsWith("ok");
        if (!wasOk && nowOk) { fixed++; continue; }
        changed++;
        std::cout << (wasOk && !nowOk ? "REGRESSED  " : "CHANGED    ")
                  << qPrintable(kv.first) << std::endl
                  << "    was: " << qPrintable(old->second) << std::endl
                  << "    now: " << qPrintable(kv.second) << std::endl;
    }
    for (const auto& kv : before)
        if (current.find(kv.first) == current.end()) missing++;

    std::cout << "--- vs baseline ---" << std::endl
              << "  regressed/changed " << changed << "   newly working " << fixed
              << "   new files " << added << "   missing files " << missing
              << std::endl;
    return changed > 0 ? 1 : 0;
}
