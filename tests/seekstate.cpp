// Does a seek leave the synthesiser holding the instruments the song asked for?
//
// This drives the REAL MidiPlayer and the REAL JJoMeSynth - not a model of them
// - because the question is about what those two do together, and every attempt
// to answer it by reading the code got a different wrong answer.
//
//   seekstate <file.mid|file.rcp> <soundfont.sf2> <seconds> [more seconds...]
//
// For each seek point it reports, per channel, the program the file asks for at
// that tick and the program TinySoundFont actually ends up holding.
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>
#include <utility>

#include "midiplayer.h"
#include "jjomesynth.h"
#include "rcpfilehandler.h"

namespace {

// What the file itself says each channel's program is at a given tick, read
// straight out of the bytes so it owes nothing to the player.
std::map<int, int> expectedPrograms(const QByteArray& in, uint32_t untilTick, int& division)
{
    std::map<int, int> prog;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(in.constData());
    const size_t n = size_t(in.size());
    if (n < 14) return prog;
    const int ntrk = (d[10] << 8) | d[11];
    division = (d[12] << 8) | d[13];

    size_t pos = 14;
    for (int t = 0; t < ntrk && pos + 8 <= n; ++t) {
        const uint32_t len = (uint32_t(d[pos+4])<<24)|(uint32_t(d[pos+5])<<16)|(uint32_t(d[pos+6])<<8)|d[pos+7];
        size_t i = pos + 8;
        const size_t end = (n < i + len) ? n : (i + len);
        uint32_t tick = 0;
        uint8_t running = 0;
        while (i < end) {
            uint32_t delta = 0;
            while (i < end) { const uint8_t b = d[i++]; delta = (delta << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
            if (i >= end) break;
            tick += delta;
            uint8_t st = d[i];
            if (st < 0x80) st = running; else { ++i; running = st; }
            if (st == 0xFF) {
                if (i >= end) break;
                ++i;                       // meta type
                uint32_t mlen = 0;
                while (i < end) { const uint8_t b = d[i++]; mlen = (mlen << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
                i += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = 0;
                while (i < end) { const uint8_t b = d[i++]; slen = (slen << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
                i += slen;
            } else {
                const int nd = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
                if ((st & 0xF0) == 0xC0 && tick <= untilTick && i < end)
                    prog[st & 0x0F] = d[i];
                i += size_t(nd);
            }
        }
        pos += 8 + len;
    }
    return prog;
}

// Seconds to ticks, walking the file's own tempo map.
uint32_t tickAt(const QByteArray& in, double seconds)
{
    const uint8_t* d = reinterpret_cast<const uint8_t*>(in.constData());
    const size_t n = size_t(in.size());
    if (n < 14) return 0;
    const int ntrk = (d[10] << 8) | d[11];
    const int division = (d[12] << 8) | d[13];
    if (division <= 0) return 0;

    std::vector<std::pair<uint32_t, uint32_t>> tempos;   // tick, us per quarter
    size_t pos = 14;
    for (int t = 0; t < ntrk && pos + 8 <= n; ++t) {
        const uint32_t len = (uint32_t(d[pos+4])<<24)|(uint32_t(d[pos+5])<<16)|(uint32_t(d[pos+6])<<8)|d[pos+7];
        size_t i = pos + 8;
        const size_t end = (n < i + len) ? n : (i + len);
        uint32_t tick = 0;
        uint8_t running = 0;
        while (i < end) {
            uint32_t delta = 0;
            while (i < end) { const uint8_t b = d[i++]; delta = (delta << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
            if (i >= end) break;
            tick += delta;
            uint8_t st = d[i];
            if (st < 0x80) st = running; else { ++i; running = st; }
            if (st == 0xFF) {
                if (i >= end) break;
                const uint8_t meta = d[i++];
                uint32_t mlen = 0;
                while (i < end) { const uint8_t b = d[i++]; mlen = (mlen << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
                if (meta == 0x51 && mlen == 3 && i + 3 <= end)
                    tempos.push_back({tick, (uint32_t(d[i])<<16)|(uint32_t(d[i+1])<<8)|d[i+2]});
                i += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = 0;
                while (i < end) { const uint8_t b = d[i++]; slen = (slen << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
                i += slen;
            } else {
                i += ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            }
        }
        pos += 8 + len;
    }
    std::sort(tempos.begin(), tempos.end());

    double us = 0.0, upq = 500000.0;
    uint32_t prev = 0;
    const double wantUs = seconds * 1000000.0;
    for (const auto& tc : tempos) {
        const double span = double(tc.first - prev) * upq / division;
        if (us + span >= wantUs) break;
        us += span; prev = tc.first; upq = double(tc.second);
    }
    return prev + uint32_t((wantUs - us) * division / upq);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        printf("usage: seekstate <file.mid|file.rcp> <soundfont.sf2> <seconds> [seconds...]\n");
        return 2;
    }
    const QString inPath = QString::fromLocal8Bit(argv[1]);
    const QString sfPath = QString::fromLocal8Bit(argv[2]);

    QFile fin(inPath);
    if (!fin.open(QIODevice::ReadOnly)) { printf("cannot read %s\n", argv[1]); return 1; }
    QByteArray raw = fin.readAll();
    fin.close();
    if (RcpFileHandler::isRcpData(raw)) {
        // The PATH overload: the QByteArray one cannot look beside the song for
        // the .GSD, so it converts without the control-file setup and the
        // expectations below then disagree with what the player is actually
        // playing. midiwav had the same bug and it cost a round of chasing.
        raw = RcpFileHandler::extractMidiData(inPath);
        if (raw.isEmpty()) { printf("RCP conversion failed\n"); return 1; }
    }

    MidiPlayer player;
    player.setUseInternalSynth(true, sfPath);
    // -1 selects the internal synth AND sets `connected`. Without it the seek
    // returns at its first line and every channel reads back as preset 0 -
    // which looks exactly like the bug being hunted. The first version of this
    // test did that and "reproduced" it perfectly.
    if (!player.connectToDevice(-1)) { printf("connect failed\n"); return 1; }
    if (!player.loadMidiFile(inPath)) { printf("load failed\n"); return 1; }

    // The application seeks while the song is PLAYING, and the playback thread
    // is still feeding events either side of the jump. Seeking from a standstill
    // is a different situation and was not the one reported.
    player.play();
    QThread::msleep(600);

    int failures = 0;
    for (int a = 3; a < argc; ++a) {
        const double seconds = atof(argv[a]);
        player.setPosition((unsigned long)(seconds * 1000.0));
        QThread::msleep(150);
        JJoMeSynth::instance().debugDrainEvents();

        int division = 48;
        // Which program the file asks for AT THE SEEK POINT, not at the end.
        // Comparing against the last one in the file marks a channel wrong
        // whenever it changes instrument later on, which it usually does.
        std::map<int, int> want = expectedPrograms(raw, tickAt(raw, seconds), division);

        printf("\n=== seek to %.1f s\n", seconds);
        printf("  ch   file wants   synth holds\n");
        for (int ch = 0; ch < 16; ++ch) {
            const int got = JJoMeSynth::instance().debugChannelPreset(ch);
            const int bank = JJoMeSynth::instance().debugChannelBank(ch);
            auto it = want.find(ch);
            if (it == want.end() && got <= 0) continue;
            const int expect = (it == want.end()) ? 0 : it->second;
            const bool bad = (it != want.end() && got != expect);
            if (bad) ++failures;
            printf("  %2d   %10d   %11d  (bank %d)%s\n",
                   ch + 1, expect, got, bank, bad ? "   <-- WRONG" : "");
        }
    }
    printf("\n%d channel(s) came back with the wrong instrument\n", failures);
    return failures ? 1 : 0;
}
