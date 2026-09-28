// Drives MldFmPlayer exactly the way MainWindow does - loadFile, play,
// renderAudio into a buffer somebody else already filled - so a fault can be
// pinned on the player or on the wiring around it, rather than guessed at.
//
// The engine itself is covered by mldrender; this covers the layer above it,
// which is where "the slider does nothing" has to live if the engine renders.
#include "../mldfmplayer.h"
#include "../jjomesynth.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>
#include <cmath>
#include <vector>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { printf("usage: mldplayer <song.mdz> [balance]\n"); return 2; }

    const QString path = QString::fromLocal8Bit(argv[1]);
    const int balance = (argc >= 3) ? atoi(argv[2]) : 50;

    MldFmPlayer p;
    const bool loaded = p.loadFile(path);
    printf("  loadFile      : %s\n", loaded ? "ok" : "FAILED");
    if (!loaded) return 1;

    printf("  isLoaded      : %s\n", p.isLoaded() ? "true" : "false");
    p.setBalance(balance);
    p.setVolume(100);
    p.play();
    printf("  isPlaying     : %s\n", p.isPlaying() ? "true" : "false");
    printf("  balance %3d   : fmGain %.3f  midiGain %.3f\n",
           balance, p.fmGain(), p.midiGain());

    // Render five seconds the way the audio callback does: a buffer that is
    // NOT cleared first, because this player mixes into whatever the SoundFont
    // or the MT-32 has already put there.
    // The player initialises its engine at the device rate JJoMeSynth reports
    // (49716 with no device open), not at a convenient constant.
    const int rate = JJoMeSynth::instance().deviceSampleRate(), frames = rate * 5, block = 512;
    std::vector<float> buf(size_t(block) * 2, 0.0f);
    double sum = 0.0; double peak = 0.0; long n = 0;
    for (int done = 0; done < frames; done += block) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        p.renderAudio(buf.data(), block);
        for (float v : buf) {
            sum += double(v) * v; ++n;
            if (fabs(v) > peak) peak = fabs(v);
        }
    }
    printf("  rendered      : peak %.4f  rms %.5f  (still playing: %s)\n",
           peak, sqrt(sum / double(n)), p.isPlaying() ? "yes" : "no");

    // The transport around the engine: a pause must hold its place, play()
    // must resume from it rather than from the top, a seek must land where it
    // was asked to, and only stop() rewinds. Pause-then-resume used to restart
    // the FM half at bar one while the MIDI half carried on - the two halves
    // "playing apart".
    auto renderSeconds = [&](double secs) {
        for (int done = 0; done < int(rate * secs); done += block) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            p.renderAudio(buf.data(), block);
        }
    };
    int failures = 0;
    auto expect = [&](const char* what, unsigned long got, unsigned long want, unsigned long tol) {
        const bool ok = (got + tol >= want) && (got <= want + tol);
        printf("  %-28s %6lu ms  (want %lu)  %s\n", what, got, want, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    };
    expect("position after 5 s", p.positionMs(), 5000, 120);
    p.pause();
    renderSeconds(1.0);                          // paused: nothing should move
    expect("held through a pause", p.positionMs(), 5000, 120);
    p.play();                                    // resume, not restart
    renderSeconds(1.0);
    expect("resumed, +1 s", p.positionMs(), 6000, 120);
    p.seekMs(20000);
    expect("seek to 20 s", p.positionMs(), 20000, 120);
    renderSeconds(1.0);
    expect("playing on from the seek", p.positionMs(), 21000, 120);
    p.stop();
    p.play();
    renderSeconds(0.5);
    expect("stop then play: from the top", p.positionMs(), 500, 120);
    return failures ? 1 : 0;
}
