// Opens every song in a folder through the same code jmp uses, and reports what
// failed. No audio device, no window - just the loaders.
//
// This exists because the project had no automated check at all. Three bugs
// shipped on 2026-08-19, and one of them only came to light because the owner
// happened to ask a question about the setting it broke. A library of 130,000
// files is the best test material this project will ever have; it was being used
// by hand, one afternoon at a time.
//
// Deliberately NOT part of the player. Separate executable, separate main(),
// built only with -DBUILD_TESTS=ON. Nothing here reaches release/.
//
//   loadall <folder> [--formats gyb,oka,ims] [--baseline FILE] [--quiet]
//
// Without --baseline it prints failures and exits non-zero if there were any.
// With one it compares against a recorded run, which is the useful mode: a
// library this size always has a few genuinely broken files, and what matters is
// whether *today's* build broke something that used to work.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QtGlobal>

#include <iostream>
#include <map>

#include "gybbackend.h"
#include "okabackend.h"
#include "nobfilehandler.h"
#include "okafilehandler.h"
#include "gybokamidi.h"

#include <adplug/adplug.h>
#include <adplug/opl.h>
#include <adplug/fprovide.h>

namespace {

// The OPL backends need somewhere to write registers. Nothing here listens, but
// the chip has to exist: with OPL3 enabled a channel whose 0xC0 stereo bits are
// clear renders silence, and a backend that cannot write at all behaves
// differently from one that can.
class SilentOpl : public Copl {
public:
    SilentOpl() { currType = TYPE_OPL3; }
    void write(int, int) override {}
    void init() override {}
};

enum class Kind { Gyb, Oka, Oksori, Nob, Ims, Midi, Unknown };

Kind kindOf(const QString& suffix)
{
    const QString s = suffix.toLower();
    if (s == "gyb") return Kind::Gyb;
    if (s == "oka") return Kind::Oka;
    if (s == "okm" || s == "okw") return Kind::Oksori;
    if (s == "nob") return Kind::Nob;
    if (s == "ims" || s == "rol" || s == "sop") return Kind::Ims;
    if (s == "mid" || s == "midi") return Kind::Midi;
    return Kind::Unknown;
}

const char* kindName(Kind k)
{
    switch (k) {
        case Kind::Gyb:     return "gyb";
        case Kind::Oka:     return "oka";
        case Kind::Oksori:  return "okm/okw";
        case Kind::Nob:     return "nob";
        case Kind::Ims:     return "ims/rol/sop";
        case Kind::Midi:    return "mid";
        case Kind::Unknown: return "?";
    }
    return "?";
}

// One line per file, so a diff against the baseline points at the file that
// changed rather than at a summary that moved.
struct Result {
    bool ok = false;
    QString detail;     // "notes=4098 inst=21" on success, the reason on failure
};

// A song that loads but has no notes plays silence, which is a failure the
// loaders themselves do not report - and exactly the shape of the bug where
// .ROL-converted files were silent because their operators had attack rate 0.
Result loadGybOka(const QString& path, bool oka)
{
    Result r;
    QFileInfo info(path);
    if (!info.exists() || info.size() == 0) { r.detail = "missing or empty"; return r; }

    SilentOpl opl;
    CProvider_Filesystem provider;

    // toUtf8, NOT QFile::encodeName, and the PATH rather than the contents -
    // exactly what GybPlayer::loadFile does and for the reason its comment
    // gives: the backend turns this narrow string back into a QString with
    // fromStdString, which is UTF-8, so encodeName's ANSI would mangle a Korean
    // filename. Passing the file's bytes instead of its name made every single
    // .GYB and .OKA "load rejected" on the first run of this tool.
    const QByteArray narrow = path.toUtf8();

    if (oka) {
        OkaBackend b(&opl);
        if (!b.load(narrow.constData(), provider)) { r.detail = "load rejected"; return r; }
        const unsigned int inst = b.getinstruments();
        r.ok = true;
        r.detail = QString("inst=%1").arg(inst);
    } else {
        GybBackend b(&opl);
        if (!b.load(narrow.constData(), provider)) { r.detail = "load rejected"; return r; }
        const unsigned int inst = b.getinstruments();
        r.ok = true;
        r.detail = QString("inst=%1").arg(inst);
    }
    return r;
}

Result loadIms(const QString& path)
{
    Result r;
    CProvider_Filesystem provider;
    SilentOpl opl;
    CPlayer* p = CAdPlug::factory(QFile::encodeName(path).constData(), &opl,
                                  CAdPlug::players, provider);
    if (!p) { r.detail = "no AdPlug player accepted it"; return r; }
    const unsigned long len = p->songlength();
    const unsigned int inst = p->getinstruments();
    delete p;
    if (len == 0) { r.detail = "length 0"; return r; }
    r.ok = true;
    r.detail = QString("len=%1ms inst=%2").arg(len).arg(inst);
    return r;
}

// The container formats: what matters is that the embedded SMF comes out. jmp
// writes it to a temp file and hands it to MidiPlayer, so an empty extraction is
// a song that will not play.
Result loadContainer(const QString& path, Kind k)
{
    Result r;
    QByteArray midi;
    if (k == Kind::Nob) {
        if (!NobFileHandler::isNobFile(path)) { r.detail = "not recognised as NOB"; return r; }
        midi = NobFileHandler::extractMidiData(path);
    } else {
        if (!OkaFileHandler::isOkaFile(path)) { r.detail = "not recognised as Oksori"; return r; }
        midi = OkaFileHandler::extractMidiData(path);
    }
    if (midi.isEmpty()) { r.detail = "no MIDI extracted"; return r; }
    if (!midi.startsWith("MThd")) { r.detail = "extracted data is not an SMF"; return r; }
    r.ok = true;
    r.detail = QString("smf=%1B").arg(midi.size());
    return r;
}

Result loadMidi(const QString& path)
{
    Result r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { r.detail = "cannot open"; return r; }
    const QByteArray head = f.read(14);
    const qint64 size = f.size();
    f.close();
    if (size < 14)                 { r.detail = "too short to be an SMF"; return r; }
    if (!head.startsWith("MThd")) { r.detail = "no MThd header"; return r; }
    r.ok = true;
    r.detail = QString("size=%1B").arg(size);
    return r;
}

// The MIDI path added in R2.7b. Reported separately from the OPL load of the
// same file, because they are different code and either can break alone.
Result buildMidiFromOpl(const QString& path)
{
    Result r;
    QString err;
    const QByteArray midi = gybokamidi::toMidi(path, gybokamidi::buildPlan(path), &err);
    if (midi.isEmpty()) {
        r.detail = err.isEmpty() ? QString("toMidi returned nothing") : err;
        return r;
    }
    r.ok = true;
    r.detail = QString("smf=%1B").arg(midi.size());
    return r;
}

}  // namespace

// The loaders are chatty - GybBackend and OkaBackend narrate every rewind - and
// 14,000 files of that buries the failures this tool exists to show. Warnings
// and above still come through.
static void quietHandler(QtMsgType type, const QMessageLogContext& ctx,
                         const QString& msg)
{
    if (type == QtDebugMsg || type == QtInfoMsg) return;
    QByteArray local = msg.toLocal8Bit();
    std::cerr << local.constData() << std::endl;
    (void)ctx;
}

int main(int argc, char** argv)
{
    qInstallMessageHandler(quietHandler);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("loadall");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Load every song under a folder through jmp's own loaders.");
    parser.addHelpOption();
    parser.addPositionalArgument("folder", "Folder to walk, recursively.");
    QCommandLineOption formatsOpt(
        "formats", "Comma-separated extensions to include, e.g. gyb,oka,ims. "
                   "Default: everything but .mid, which is usually 100k+ files.",
        "list", "gyb,oka,okm,okw,nob,ims,rol,sop");
    QCommandLineOption baselineOpt(
        "baseline", "Compare against this recorded run, and report only what "
                    "changed. Written if it does not exist yet.", "file");
    QCommandLineOption quietOpt("quiet", "Only print the summary and any changes.");
    parser.addOption(formatsOpt);
    parser.addOption(baselineOpt);
    parser.addOption(quietOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) { parser.showHelp(2); }

    const QString root = args.first();
    if (!QFileInfo(root).isDir()) {
        std::cerr << "not a folder: " << qPrintable(root) << std::endl;
        return 2;
    }

    QStringList wanted;
    for (const QString& e : parser.value(formatsOpt).split(',', Qt::SkipEmptyParts))
        wanted << "*." + e.trimmed().toLower();

    const bool quiet = parser.isSet(quietOpt);

    QElapsedTimer clock;
    clock.start();

    // path -> "ok detail" or "FAIL reason", sorted so the output is stable and a
    // diff against the baseline is readable.
    std::map<QString, QString> current;
    std::map<Kind, QPair<int, int>> perKind;   // kind -> (ok, failed)

    QDirIterator it(root, wanted, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const Kind k = kindOf(QFileInfo(path).suffix());
        if (k == Kind::Unknown) continue;

        Result r;
        switch (k) {
            case Kind::Gyb:    r = loadGybOka(path, false); break;
            case Kind::Oka:    r = loadGybOka(path, true);  break;
            case Kind::Oksori: r = loadContainer(path, k);  break;
            case Kind::Nob:    r = loadContainer(path, k);  break;
            case Kind::Ims:    r = loadIms(path);           break;
            case Kind::Midi:   r = loadMidi(path);          break;
            default: break;
        }

        // Songs the OPL engine accepts should also convert for MIDI playback.
        if (r.ok && (k == Kind::Gyb || k == Kind::Oka)) {
            const Result m = buildMidiFromOpl(path);
            if (!m.ok) { r.ok = false; r.detail = "MIDI build: " + m.detail; }
            else       { r.detail += "  " + m.detail; }
        }

        const QString rel = QDir(root).relativeFilePath(path);
        current[rel] = (r.ok ? "ok   " : "FAIL ") + r.detail;

        auto& tally = perKind[k];
        if (r.ok) tally.first++; else tally.second++;

        if (!r.ok && !quiet)
            std::cout << "FAIL  " << qPrintable(rel) << "   " << qPrintable(r.detail)
                      << std::endl;
    }

    int okTotal = 0, failTotal = 0;
    for (const auto& kv : perKind) { okTotal += kv.second.first; failTotal += kv.second.second; }

    std::cout << std::endl << "--- summary ---" << std::endl;
    for (const auto& kv : perKind)
        std::cout << "  " << kindName(kv.first) << "  ok " << kv.second.first
                  << "  failed " << kv.second.second << std::endl;
    std::cout << "  total " << (okTotal + failTotal) << " files, " << failTotal
              << " failed, " << clock.elapsed() / 1000.0 << " s" << std::endl;

    // Baseline comparison. A library this size has a few files that were always
    // broken; a regression is a file that used to load and now does not.
    const QString baselinePath = parser.value(baselineOpt);
    if (baselinePath.isEmpty())
        return failTotal > 0 ? 1 : 0;

    if (!QFile::exists(baselinePath)) {
        QFile out(baselinePath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            std::cerr << "cannot write baseline: " << qPrintable(baselinePath) << std::endl;
            return 2;
        }
        QTextStream t(&out);
        t << "# loadall baseline  " << QDateTime::currentDateTime().toString(Qt::ISODate)
          << "  " << (okTotal + failTotal) << " files under " << root << Qt::endl;
        for (const auto& kv : current) t << kv.first << "\t" << kv.second << Qt::endl;
        std::cout << "baseline written: " << qPrintable(baselinePath) << std::endl;
        return 0;
    }

    std::map<QString, QString> before;
    {
        QFile in(baselinePath);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "cannot read baseline" << std::endl;
            return 2;
        }
        QTextStream t(&in);
        while (!t.atEnd()) {
            const QString line = t.readLine();
            if (line.startsWith('#') || line.isEmpty()) continue;
            const int tab = line.indexOf('\t');
            if (tab > 0) before[line.left(tab)] = line.mid(tab + 1);
        }
    }

    int regressed = 0, fixed = 0, added = 0, removed = 0;
    for (const auto& kv : current) {
        auto old = before.find(kv.first);
        if (old == before.end()) { added++; continue; }
        if (old->second == kv.second) continue;
        const bool wasOk = old->second.startsWith("ok");
        const bool nowOk = kv.second.startsWith("ok");
        if (wasOk && !nowOk) {
            regressed++;
            std::cout << "REGRESSED  " << qPrintable(kv.first) << std::endl
                      << "    was: " << qPrintable(old->second) << std::endl
                      << "    now: " << qPrintable(kv.second) << std::endl;
        } else if (!wasOk && nowOk) {
            fixed++;
        } else {
            // Both loaded, but a number moved - an instrument count or an SMF
            // size. Not a failure, but it means the output changed.
            regressed++;
            std::cout << "CHANGED    " << qPrintable(kv.first) << std::endl
                      << "    was: " << qPrintable(old->second) << std::endl
                      << "    now: " << qPrintable(kv.second) << std::endl;
        }
    }
    for (const auto& kv : before)
        if (current.find(kv.first) == current.end()) removed++;

    std::cout << "--- vs baseline ---" << std::endl
              << "  regressed/changed " << regressed
              << "   newly working " << fixed
              << "   new files " << added
              << "   missing files " << removed << std::endl;

    return regressed > 0 ? 1 : 0;
}
