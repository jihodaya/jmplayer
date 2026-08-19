// Renders songs to audio through the real players and hashes the result, so a
// change in *sound* is caught. That is the one thing that matters most here and
// the one thing loadall cannot see: a file can load perfectly and play wrong.
//
// It goes through GybPlayer / OkaPlayer / ImsPlayer::renderAudio(), the same
// function the audio device calls, rather than driving the backends directly.
// All three are independent of miniaudio - nothing here opens a device - so the
// test exercises the players' own clocking, seeking guards, DSP and stereo
// placement instead of a simplified copy of them. A harness that rebuilt that
// by hand would drift from the player and stop meaning anything.
//
// Deliberately NOT part of the player: separate main(), -DBUILD_TESTS=ON only.
//
//   render <manifest> [--baseline FILE] [--seconds N] [--write-wav DIR]
//
// The manifest is one song path per line; # comments and blank lines ignored.
// Relative paths resolve against the manifest's own folder, so the list can sit
// next to the songs.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QtGlobal>

#include <iostream>
#include <map>
#include <vector>

#include "gybplayer.h"
#include "okaplayer.h"
#include "imsplayer.h"

namespace {

constexpr unsigned int kSampleRate = 48000;
constexpr unsigned int kBlockFrames = 512;    // what the device asks for

void quietHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (type == QtDebugMsg || type == QtInfoMsg) return;
    std::cerr << msg.toLocal8Bit().constData() << std::endl;
}

enum class Engine { Gyb, Oka, Ims, Unsupported };

Engine engineFor(const QString& path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    if (s == "gyb") return Engine::Gyb;
    if (s == "oka") return Engine::Oka;
    if (s == "ims" || s == "rol" || s == "sop") return Engine::Ims;
    return Engine::Unsupported;
}

struct Rendered {
    bool ok = false;
    QString detail;
    QByteArray pcm;       // interleaved stereo float, only kept for --write-wav
    QString digest;
};

// Silence is the failure this is really looking for. A hash alone would happily
// certify a song that renders nothing at all - which is exactly what the
// name-only .ROL-converted files used to do, their operators never rising
// because attack rate was 0 - so the peak and a rough loudness go into the
// recorded line beside the hash.
Rendered renderOne(const QString& path, double seconds, bool keepPcm)
{
    Rendered out;
    const unsigned int total = unsigned(seconds * kSampleRate);

    // One player per song. They are cheap, and reusing one across songs would
    // make the result depend on the order the manifest happens to be in.
    GybPlayer gyb;
    OkaPlayer oka;
    ImsPlayer ims;

    bool loaded = false;
    switch (engineFor(path)) {
        case Engine::Gyb: loaded = gyb.loadFile(path); break;
        case Engine::Oka: loaded = oka.loadFile(path); break;
        case Engine::Ims: loaded = ims.loadFile(path); break;
        default: out.detail = "no engine for this extension"; return out;
    }
    if (!loaded) { out.detail = "load failed"; return out; }

    switch (engineFor(path)) {
        case Engine::Gyb: gyb.play(); break;
        case Engine::Oka: oka.play(); break;
        case Engine::Ims: ims.play(); break;
        default: break;
    }

    std::vector<float> block(kBlockFrames * 2);
    QCryptographicHash hash(QCryptographicHash::Md5);
    float peak = 0.0f;
    double energy = 0.0;
    unsigned int done = 0;

    while (done < total) {
        const unsigned int want = qMin(kBlockFrames, total - done);
        std::fill(block.begin(), block.end(), 0.0f);
        switch (engineFor(path)) {
            case Engine::Gyb: gyb.renderAudio(block.data(), want); break;
            case Engine::Oka: oka.renderAudio(block.data(), want); break;
            case Engine::Ims: ims.renderAudio(block.data(), want); break;
            default: break;
        }
        const int bytes = int(want * 2 * sizeof(float));
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(block.data()), bytes));
        if (keepPcm)
            out.pcm.append(reinterpret_cast<const char*>(block.data()), bytes);
        for (unsigned int i = 0; i < want * 2; ++i) {
            const float v = block[i];
            peak = qMax(peak, qAbs(v));
            energy += double(v) * double(v);
        }
        done += want;
    }

    const double rms = total ? qSqrt(energy / (total * 2)) : 0.0;
    out.digest = QString::fromLatin1(hash.result().toHex().left(16));
    out.ok = true;
    out.detail = QString("md5=%1 peak=%2 rms=%3")
                     .arg(out.digest)
                     .arg(peak, 0, 'f', 4)
                     .arg(rms, 0, 'f', 5);
    if (peak == 0.0f) { out.ok = false; out.detail += "  SILENT"; }
    return out;
}

// 32-bit float WAV, so a changed hash can be listened to rather than guessed at.
bool writeWav(const QString& path, const QByteArray& pcm)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 dataBytes = quint32(pcm.size());
    auto u32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);  u32(36 + dataBytes);  f.write("WAVE", 4);
    f.write("fmt ", 4);  u32(16);
    u16(3);                              // IEEE float
    u16(2);                              // stereo
    u32(kSampleRate);
    u32(kSampleRate * 2 * 4);            // byte rate
    u16(2 * 4);                          // block align
    u16(32);                             // bits
    f.write("data", 4);  u32(dataBytes);
    f.write(pcm);
    f.close();
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    qInstallMessageHandler(quietHandler);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("render");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Render songs through jmp's own players and hash the audio.");
    parser.addHelpOption();
    parser.addPositionalArgument("manifest", "File listing one song path per line.");
    QCommandLineOption baselineOpt("baseline",
        "Compare against this recorded run; written if absent.", "file");
    QCommandLineOption secondsOpt("seconds",
        "How much of each song to render. Longer catches later differences and "
        "costs proportionally more time.", "n", "20");
    QCommandLineOption wavOpt("write-wav",
        "Also write each render as a 32-bit float WAV into this folder, so a "
        "changed hash can be listened to.", "dir");
    parser.addOption(baselineOpt);
    parser.addOption(secondsOpt);
    parser.addOption(wavOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) parser.showHelp(2);

    const QString manifestPath = args.first();
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "cannot read manifest: " << qPrintable(manifestPath) << std::endl;
        return 2;
    }
    const QDir manifestDir = QFileInfo(manifestPath).absoluteDir();

    QStringList songs;
    {
        QTextStream t(&manifest);
        while (!t.atEnd()) {
            const QString line = t.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            songs << (QFileInfo(line).isAbsolute() ? line
                                                   : manifestDir.filePath(line));
        }
    }
    manifest.close();
    if (songs.isEmpty()) {
        std::cerr << "manifest lists no songs" << std::endl;
        return 2;
    }

    const double seconds = parser.value(secondsOpt).toDouble();
    const QString wavDir = parser.value(wavOpt);
    if (!wavDir.isEmpty()) QDir().mkpath(wavDir);

    std::map<QString, QString> current;
    int failed = 0;

    for (const QString& song : songs) {
        const QString key = QFileInfo(song).fileName();
        if (!QFileInfo::exists(song)) {
            current[key] = "MISSING";
            std::cout << "MISSING  " << qPrintable(key) << std::endl;
            failed++;
            continue;
        }
        Rendered r = renderOne(song, seconds, !wavDir.isEmpty());
        current[key] = (r.ok ? "ok   " : "FAIL ") + r.detail;
        if (!r.ok) {
            failed++;
            std::cout << "FAIL     " << qPrintable(key) << "   "
                      << qPrintable(r.detail) << std::endl;
        } else {
            std::cout << "ok       " << qPrintable(key) << "   "
                      << qPrintable(r.detail) << std::endl;
        }
        if (!wavDir.isEmpty() && !r.pcm.isEmpty())
            writeWav(QDir(wavDir).filePath(key + ".wav"), r.pcm);
    }

    std::cout << std::endl << "--- summary ---" << std::endl
              << "  " << songs.size() << " songs, " << failed << " failed, "
              << seconds << " s each" << std::endl;

    const QString baselinePath = parser.value(baselineOpt);
    if (baselinePath.isEmpty()) return failed > 0 ? 1 : 0;

    if (!QFile::exists(baselinePath)) {
        QFile out(baselinePath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            std::cerr << "cannot write baseline" << std::endl;
            return 2;
        }
        QTextStream t(&out);
        t << "# render baseline  " << QDateTime::currentDateTime().toString(Qt::ISODate)
          << "  " << songs.size() << " songs, " << seconds << " s each, "
          << kSampleRate << " Hz" << Qt::endl;
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
            if (line.isEmpty() || line.startsWith('#')) continue;
            const int tab = line.indexOf('\t');
            if (tab > 0) before[line.left(tab)] = line.mid(tab + 1);
        }
    }

    int changed = 0, added = 0;
    for (const auto& kv : current) {
        auto old = before.find(kv.first);
        if (old == before.end()) { added++; continue; }
        if (old->second == kv.second) continue;
        changed++;
        std::cout << "CHANGED  " << qPrintable(kv.first) << std::endl
                  << "    was: " << qPrintable(old->second) << std::endl
                  << "    now: " << qPrintable(kv.second) << std::endl;
    }

    std::cout << "--- vs baseline ---" << std::endl
              << "  changed " << changed << "   new " << added << std::endl;
    if (changed)
        std::cout << "  Re-run with --write-wav to hear what moved." << std::endl;

    return changed > 0 ? 1 : 0;
}
