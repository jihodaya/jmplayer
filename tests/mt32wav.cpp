// Renders a Standard MIDI File through jmp's built-in MT-32 (munt) to a WAV,
// so the level of a song's MIDI half on that device can be measured against
// the OPM half the MLD engine renders - which is how "the volume slider does
// nothing on NEW_Wa, the FM slider takes everything away" was sized.
//
//   mt32wav <in.mid> <out.wav> [seconds] [romset-id]
//
// ROMs are read from MT32ROMs\ beside the exe, as the player does.
#include "../mt32synth.h"

#include <QApplication>
#include <QFile>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

struct Ev { unsigned long tick; int order; std::vector<unsigned char> bytes; int tempo = -1; };

unsigned long vlq(const unsigned char* d, int n, int& i)
{
    unsigned long v = 0;
    while (i < n) { const unsigned char c = d[i++]; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) break; }
    return v;
}

void writeWav(const char* path, const std::vector<float>& pcm, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const unsigned dataBytes = unsigned(pcm.size() * 2);
    auto u32 = [&](unsigned v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](unsigned short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(unsigned(rate)); u32(unsigned(rate) * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(dataBytes);
    for (float s : pcm) {
        const int v = int(std::max(-1.0f, std::min(1.0f, s)) * 32767.0f);
        const short sv = short(v);
        fwrite(&sv, 2, 1, f);
    }
    fclose(f);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc < 3) { printf("usage: mt32wav <in.mid> <out.wav> [seconds] [romset-id]\n"); return 2; }
    const double seconds = (argc >= 4) ? atof(argv[3]) : 60.0;
    const QString romId = (argc >= 5) ? QString::fromLocal8Bit(argv[4]) : QString();

    QFile fin(QString::fromLocal8Bit(argv[1]));
    if (!fin.open(QIODevice::ReadOnly)) { printf("cannot read %s\n", argv[1]); return 1; }
    const QByteArray raw = fin.readAll();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(raw.constData());
    const int n = raw.size();
    if (n < 14 || memcmp(d, "MThd", 4) != 0) { printf("not an SMF\n"); return 1; }
    const int ntr = (d[10] << 8) | d[11];
    const int division = (d[12] << 8) | d[13];

    std::vector<Ev> ev;
    int p = 14, order = 0;
    for (int t = 0; t < ntr && p + 8 <= n; ++t) {
        const int len = (d[p + 4] << 24) | (d[p + 5] << 16) | (d[p + 6] << 8) | d[p + 7];
        int i = p + 8; const int end = std::min(n, i + len);
        unsigned long tick = 0; unsigned char run = 0;
        while (i < end) {
            tick += vlq(d, end, i);
            if (i >= end) break;
            unsigned char st = d[i];
            if (st == 0xFF) {
                const int type = d[i + 1]; i += 2;
                const int l = int(vlq(d, end, i));
                if (type == 0x51 && l == 3) {
                    Ev e; e.tick = tick; e.order = order++; e.tempo = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
                    ev.push_back(e);
                }
                i += l; continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                ++i; const int l = int(vlq(d, end, i));
                Ev e; e.tick = tick; e.order = order++;
                e.bytes.push_back(0xF0);
                for (int k = 0; k < l && i + k < end; ++k) e.bytes.push_back(d[i + k]);
                if (e.bytes.back() != 0xF7) e.bytes.push_back(0xF7);
                ev.push_back(e); i += l; continue;
            }
            if (st & 0x80) { run = st; ++i; } else st = run;
            const int nb = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            Ev e; e.tick = tick; e.order = order++;
            e.bytes.push_back(st);
            for (int k = 0; k < nb && i + k < end; ++k) e.bytes.push_back(d[i + k]);
            ev.push_back(e); i += nb;
        }
        p = 8 + p + len;
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.tick < b.tick; });

    Mt32Synth synth;
    if (!synth.Open(romId)) { printf("MT-32 did not open: %s\n", qPrintable(synth.ErrorString())); return 1; }
    printf("romset %s\n", qPrintable(synth.CurrentRomId()));
    synth.SetMasterVolume(127 * 100 / 127);

    const int rate = int(Mt32Synth::kOutputSampleRate);
    const long total = long(seconds * rate);
    std::vector<float> pcm(size_t(total) * 2, 0.0f);
    double usPerTick = 500000.0 / division, now = 0.0;   // in samples
    unsigned long lastTick = 0; long done = 0;
    size_t k = 0;
    while (done < total) {
        const double nextAt = (k < ev.size())
            ? now + double(ev[k].tick - lastTick) * usPerTick * rate / 1e6 : double(total);
        const long upto = std::min(total, long(nextAt));
        while (done < upto) {
            const unsigned chunk = unsigned(std::min<long>(upto - done, 512));
            synth.Render(pcm.data() + done * 2, chunk);
            done += chunk;
        }
        if (k >= ev.size()) break;
        now = nextAt; lastTick = ev[k].tick;
        const Ev& e = ev[k++];
        if (e.tempo >= 0) usPerTick = double(e.tempo) / division;
        else if (e.bytes[0] == 0xF0) synth.SendSysEx(e.bytes);
        else synth.SendShort(e.bytes[0], e.bytes.size() > 1 ? e.bytes[1] : 0, e.bytes.size() > 2 ? e.bytes[2] : 0);
    }

    double peak = 0, sum = 0;
    for (float s : pcm) { peak = std::max(peak, double(std::fabs(s))); sum += double(s) * s; }
    printf("%s  %.0fs  peak %.4f  rms %.5f\n", argv[2], seconds, peak, std::sqrt(sum / double(pcm.size())));
    writeWav(argv[2], pcm, rate);
    return 0;
}
