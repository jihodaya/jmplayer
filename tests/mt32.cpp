// Exercises the MT-32 engine without a window or an audio device.
//
// Everything the feature depends on is here except the two things a person has
// to judge - whether it sounds right, and whether the display looks right - so
// this is what makes "it builds" into "it works": the ROM scan finds real
// machines, a synth opens on them, notes reach it, PCM comes back with signal
// in it, and the emulated LCD says what the hardware's would.
//
// Deliberately NOT part of the player: separate main(), -DBUILD_TESTS=ON only.
//
//   mt32 [--roms DIR] [--seconds N] [--write-wav FILE]
//
// --roms points at a folder of MT-32/CM-32L ROMs. Without it the tool looks in
// the MT32ROMs folder beside itself, which is empty in a fresh build tree, and
// says so rather than failing.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <cmath>
#include <iostream>
#include <vector>

#include "mt32synth.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const QString& what, const QString& detail = QString())
{
    ++g_checks;
    std::cout << (ok ? "ok    " : "FAIL  ") << qPrintable(what);
    if (!detail.isEmpty())
        std::cout << "   " << qPrintable(detail);
    std::cout << std::endl;
    if (!ok) ++g_failed;
}

// 32-bit float WAV, so a suspicious render can be listened to rather than
// argued about - the same escape hatch tests/render.cpp offers.
bool writeWav(const QString& path, const std::vector<float>& pcm, unsigned rate)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 dataBytes = quint32(pcm.size() * sizeof(float));
    auto u32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);  u32(36 + dataBytes);  f.write("WAVE", 4);
    f.write("fmt ", 4);  u32(16);
    u16(3); u16(2); u32(rate); u32(rate * 2 * 4); u16(2 * 4); u16(32);
    f.write("data", 4);  u32(dataBytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("mt32");

    QCommandLineParser parser;
    parser.setApplicationDescription("Drive the MT-32 engine with no window and no audio device.");
    parser.addHelpOption();
    QCommandLineOption romsOpt("roms", "Folder holding MT-32 / CM-32L ROMs.", "dir");
    QCommandLineOption secsOpt("seconds", "How much audio to render per machine.", "n", "2");
    QCommandLineOption wavOpt("write-wav", "Write the last render here as a WAV.", "file");
    parser.addOption(romsOpt);
    parser.addOption(secsOpt);
    parser.addOption(wavOpt);
    parser.process(app);

    // The engine looks beside the executable; point it somewhere else by
    // copying, since the folder is baked into Mt32Synth on purpose (the player
    // must not be able to load ROMs from anywhere it likes).
    const QString romsFrom = parser.value(romsOpt);
    if (!romsFrom.isEmpty()) {
        const QString into = Mt32Synth::InstallDir();
        QDir().mkpath(into);
        const QFileInfoList src = QDir(romsFrom).entryInfoList(QDir::Files);
        int copied = 0;
        for (const QFileInfo& fi : src) {
            const QString dst = QDir(into).filePath(fi.fileName());
            if (!QFileInfo::exists(dst) && QFile::copy(fi.absoluteFilePath(), dst))
                ++copied;
        }
        std::cout << "copied " << copied << " file(s) into " << qPrintable(into) << std::endl;
    }

    std::cout << "ROM folder: " << qPrintable(Mt32Synth::InstallDir()) << std::endl << std::endl;

    const QVector<Mt32Synth::RomSet> sets = Mt32Synth::ScanRomSets();
    if (sets.isEmpty()) {
        std::cout << qPrintable(Mt32Synth::UnavailableReason()) << std::endl;
        std::cout << std::endl << "Nothing to test. Pass --roms with a folder of ROMs."
                  << std::endl;
        return 2;
    }

    std::cout << "--- machines found ---" << std::endl;
    for (const Mt32Synth::RomSet& s : sets) {
        std::cout << "  " << qPrintable(s.label.leftJustified(22))
                  << qPrintable(QFileInfo(s.controlPath).fileName()) << "  +  "
                  << qPrintable(QFileInfo(s.pcmPath).fileName()) << std::endl;
    }
    std::cout << std::endl;

    check(!Mt32Synth::UnavailableReason().isEmpty() == false,
          "UnavailableReason is empty once ROMs are present");

    const double seconds = parser.value(secsOpt).toDouble();
    const unsigned rate  = unsigned(Mt32Synth::kOutputSampleRate);
    const unsigned total = unsigned(seconds * rate);

    std::vector<float> lastRender;

    for (const Mt32Synth::RomSet& s : sets) {
        std::cout << std::endl << "=== " << qPrintable(s.label) << " ===" << std::endl;

        Mt32Synth synth;
        check(synth.Open(s.id), "opens", s.id + "  " + synth.ErrorString());
        if (!synth.IsOpen())
            continue;

        check(synth.CurrentRomId() == s.id, "reports the machine it opened",
              synth.CurrentRomId());

        // The display should say something the moment it is up - a real unit
        // shows its master volume screen.
        const QString bootText = synth.DisplayText();
        check(!bootText.trimmed().isEmpty(), "LCD says something at power-on",
              "\"" + bootText + "\"");

        // Which MIDI channels does it actually listen to?
        //
        // Not a rhetorical question: an MT-32 out of the box assigns its eight
        // parts to channels 2-9 and rhythm to 10, so a note on channel 1 - the
        // first thing a GM file uses - is simply ignored. The first version of
        // this test sent one there, measured silence, and looked like a broken
        // engine. Sweeping is both the check and the documentation.
        constexpr unsigned kBlock = 512;
        std::vector<float> probe(kBlock * 2, 0.0f);
        QString sounding;
        int firstSounding = -1;

        for (int ch = 0; ch < 16; ++ch) {
            synth.AllSoundOff();
            for (int i = 0; i < 40; ++i) synth.Render(probe.data(), kBlock);   // settle

            synth.SendShort(0x90 | ch, 60, 100);

            double peak = 0.0;
            for (int i = 0; i < 60; ++i) {
                synth.Render(probe.data(), kBlock);
                for (float v : probe) peak = qMax(peak, double(qAbs(v)));
            }
            synth.SendShort(0x80 | ch, 60, 0);

            if (peak > 0.001) {
                sounding += QString::number(ch + 1) + " ";
                if (firstSounding < 0) firstSounding = ch;
            }
        }
        check(firstSounding >= 0, "some MIDI channel produces sound",
              "channels that sound: " + sounding.trimmed());

        // The rest of the test uses a channel this machine is actually
        // listening on, rather than assuming one.
        const int ch = firstSounding >= 0 ? firstSounding : 1;

        synth.AllSoundOff();
        for (int i = 0; i < 40; ++i) synth.Render(probe.data(), kBlock);
        for (unsigned char note : { 60, 64, 67 })
            synth.SendShort(0x90 | ch, note, 100);

        std::vector<float> pcm(size_t(total) * 2, 0.0f);
        for (unsigned done = 0; done < total; done += kBlock) {
            const unsigned want = qMin(kBlock, total - done);
            synth.Render(pcm.data() + size_t(done) * 2, want);
        }

        double peak = 0.0, energy = 0.0;
        for (float v : pcm) { peak = qMax(peak, double(qAbs(v))); energy += double(v) * v; }
        const double rms = std::sqrt(energy / pcm.size());

        check(peak > 0.001, "renders audible signal",
              QString("peak %1  rms %2").arg(peak, 0, 'f', 4).arg(rms, 0, 'f', 5));

        // Silence after an all-sound-off is the other half of the same claim:
        // if the buffer were simply uninitialised the first check would pass
        // and this one would not.
        synth.AllSoundOff();
        std::vector<float> tail(size_t(rate) * 2, 0.0f);   // one second
        for (unsigned done = 0; done < rate; done += kBlock) {
            const unsigned want = qMin(kBlock, rate - done);
            synth.Render(tail.data() + size_t(done) * 2, want);
        }
        double tailPeak = 0.0;
        // Skip the first 200 ms: the envelopes have to release, and a real
        // MT-32 does not cut off instantly either.
        for (size_t i = size_t(rate * 0.2) * 2; i < tail.size(); ++i)
            tailPeak = qMax(tailPeak, double(qAbs(tail[i])));
        check(tailPeak < peak * 0.1, "goes quiet after all-sound-off",
              QString("tail peak %1 against %2").arg(tailPeak, 0, 'f', 5).arg(peak, 0, 'f', 4));

        // A display SysEx is how these files put a title on screen, and it is
        // the one piece of the panel that comes from the song rather than the
        // machine. Address 0x200000 is the MT-32's display buffer.
        const char* kMsg = "JMP TEST 1234";
        std::vector<unsigned char> sysex = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00 };
        int sum = 0x20 + 0x00 + 0x00;
        for (const char* p = kMsg; *p; ++p) {
            sysex.push_back((unsigned char) *p);
            sum += (unsigned char) *p;
        }
        sysex.push_back((unsigned char) ((128 - (sum & 0x7F)) & 0x7F));
        sysex.push_back(0xF7);
        synth.SendSysEx(sysex);

        // The display is updated during rendering, so give it some.
        std::vector<float> spin(kBlock * 2, 0.0f);
        for (int i = 0; i < 200; ++i)
            synth.Render(spin.data(), kBlock);

        const QString shown = synth.DisplayText();
        check(shown.contains("JMP TEST"), "shows a display SysEx from the song",
              "\"" + shown + "\"");

        lastRender = pcm;
        synth.Close();
        check(!synth.IsOpen(), "closes");

        // Rendering after Close must be silence rather than a crash - the audio
        // callback can be inside Render() when the user switches device.
        std::vector<float> afterClose(kBlock * 2, 1.0f);
        synth.Render(afterClose.data(), kBlock);
        bool silent = true;
        for (float v : afterClose) if (v != 0.0f) { silent = false; break; }
        check(silent, "renders silence once closed");
    }

    // Master volume, which is what the panel shows. The point is that jmp's
    // slider and the MT-32's display agree - they did not, and CC#7 could never
    // have made them, because that is per-part volume.
    {
        Mt32Synth synth;
        if (synth.Open(sets.first().id)) {
            std::vector<float> spin(1024, 0.0f);
            auto settle = [&] { for (int i = 0; i < 120; ++i) synth.Render(spin.data(), 512); };

            // Let the power-on banner clear first. A real MT-32 shows
            // " ** Roland MT-32 ** " for a moment and only then the master
            // volume screen, so a volume set into that window is applied but
            // not yet displayed - which is a fact about the machine, not a
            // fault, and cost this test a failure before it was allowed for.
            settle();

            struct Case { int slider; const char* expect; };
            const Case cases[] = { { 127, "100" }, { 64, "50" }, { 0, "0" } };

            std::cout << std::endl << "=== master volume ===" << std::endl;

            // The case the player actually hits: volume set the instant the
            // machine is opened, while it is still showing its power-on banner.
            // A real MT-32 is not listening yet at that point, and if the
            // emulation is faithful about that the setting is simply lost -
            // which is what "the volume is wrong right after it turns on" would
            // mean (reported 2026-08-21).
            {
                Mt32Synth fresh;
                if (fresh.Open(sets.first().id)) {
                    fresh.SetMasterVolume(64);          // -> 50
                    std::vector<float> s2(1024, 0.0f);
                    for (int i = 0; i < 400; ++i) fresh.Render(s2.data(), 512);
                    const QString shown = fresh.DisplayText();
                    check(shown.contains("50"),
                          "volume set during the power-on banner still takes",
                          "\"" + shown + "\"");
                    fresh.Close();
                }
            }

            // ...and does the panel ever show the wrong number on the way
            // there? A value that is right after a second is still wrong if a
            // different one flashes up first.
            {
                Mt32Synth fresh;
                if (fresh.Open(sets.first().id)) {
                    fresh.SetMasterVolume(64);          // -> 50
                    std::vector<float> s2(1024, 0.0f);
                    QString last;
                    bool sawOther = false;
                    QString firstVolScreen;
                    for (int i = 0; i < 400; ++i) {
                        fresh.Render(s2.data(), 512);
                        const QString now = fresh.DisplayText();
                        if (now == last) continue;
                        last = now;
                        if (now.contains("vol:")) {
                            if (firstVolScreen.isEmpty()) firstVolScreen = now;
                            if (!now.contains("50")) sawOther = true;
                        }
                    }
                    check(!sawOther,
                          "and no other volume is shown on the way",
                          "first volume screen: \"" + firstVolScreen + "\"");
                    fresh.Close();
                }
            }

            // Does it do anything to the SOUND, or only to the display?
            //
            // A number that tracks the slider while the loudness does not would
            // be the most literal reading of "the volume does not match", and
            // nothing above would have caught it - every check so far reads the
            // panel.
            {
                auto peakAt = [&](int slider) {
                    Mt32Synth s;
                    if (!s.Open(sets.first().id)) return 0.0;
                    std::vector<float> buf(1024, 0.0f);
                    for (int i = 0; i < 120; ++i) s.Render(buf.data(), 512);  // settle
                    s.SetMasterVolume(slider);
                    for (int i = 0; i < 60; ++i) s.Render(buf.data(), 512);
                    s.SendShort(0x91, 60, 100);                                // channel 2
                    double peak = 0.0;
                    for (int i = 0; i < 120; ++i) {
                        s.Render(buf.data(), 512);
                        for (float v : buf) peak = qMax(peak, double(qAbs(v)));
                    }
                    s.Close();
                    return peak;
                };

                const double loud  = peakAt(127);
                const double quiet = peakAt(32);
                check(loud > 0.0 && quiet < loud * 0.6,
                      "the master volume actually changes the level",
                      QString("peak %1 at full, %2 at a quarter")
                          .arg(loud, 0, 'f', 4).arg(quiet, 0, 'f', 4));
            }

            for (const Case& c : cases) {
                synth.SetMasterVolume(c.slider);
                settle();
                const QString shown = synth.DisplayText();
                check(shown.contains(QString::fromLatin1(c.expect)),
                      QString("slider %1 shows %2 on the panel").arg(c.slider).arg(c.expect),
                      "\"" + shown + "\"");
            }
            synth.Close();
        }
    }

    const QString wav = parser.value(wavOpt);
    if (!wav.isEmpty() && !lastRender.empty()) {
        if (writeWav(wav, lastRender, rate))
            std::cout << std::endl << "wrote " << qPrintable(wav) << std::endl;
    }

    std::cout << std::endl << "--- summary ---" << std::endl
              << "  " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
