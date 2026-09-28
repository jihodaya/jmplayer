// Renders a .MID (or a Recomposer file, converted first) to a WAV through
// TinySoundFont, so two files can be compared as audio rather than as event
// lists.
//
// This is a COMPARISON instrument, not a model of the player. It sequences the
// file itself instead of driving MidiPlayer, which is real-time and Qt-threaded
// and cannot be stepped offline. That is acceptable for one reason only: both
// sides of a comparison go through this same code, so whatever it gets wrong
// cancels out. Do not read a result from it as a statement about how the
// application sounds.
//
//   midiwav <in.mid|in.rcp> <out.wav> <soundfont.sf2> [seconds]
//                [--from S]   start at S seconds, applying every event before
//                             it in order first - a perfect state chase, and so
//                             the answer a seek is supposed to arrive at
//                [--chase S]  start at S seconds having rebuilt the state the
//                             way MidiPlayer::performSeekImmediate does, from a
//                             per-channel snapshot
//
// Rendering the same moment both ways and comparing is how the seek chase is
// checked: --from is what the music should sound like there, --chase is what
// the player actually does.
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tsf.h"
#include "rcpfilehandler.h"

namespace {

struct Ev {
    uint32_t tick;
    uint8_t  status, d1, d2;
    uint32_t tempo;
    bool     isTempo;
};

uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t rd16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

bool parseSmf(const QByteArray& in, std::vector<Ev>& out, int& division)
{
    const uint8_t* d = reinterpret_cast<const uint8_t*>(in.constData());
    const size_t n = size_t(in.size());
    if (n < 14 || memcmp(d, "MThd", 4) != 0) return false;
    const int ntrk = rd16(d + 10);
    division = rd16(d + 12);
    if (division <= 0) return false;

    size_t pos = 14;
    for (int t = 0; t < ntrk && pos + 8 <= n; ++t) {
        if (memcmp(d + pos, "MTrk", 4) != 0) break;
        const uint32_t len = rd32(d + pos + 4);
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
                if (meta == 0x51 && mlen == 3 && i + 3 <= end) {
                    Ev e; e.tick = tick; e.status = 0; e.d1 = 0; e.d2 = 0;
                    e.tempo = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8) | d[i + 2];
                    e.isTempo = true;
                    out.push_back(e);
                }
                i += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                uint32_t slen = 0;
                while (i < end) { const uint8_t b = d[i++]; slen = (slen << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
                i += slen;      // the SoundFont path ignores SysEx
            } else {
                const int nd = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
                Ev e;
                e.tick = tick; e.status = st;
                e.d1 = (i < end) ? d[i] : uint8_t(0);
                e.d2 = (nd == 2 && i + 1 < end) ? d[i + 1] : uint8_t(0);
                e.tempo = 0; e.isTempo = false;
                i += size_t(nd);
                out.push_back(e);
            }
        }
        pos += 8 + len;
    }
    // Events sharing a tick are ordered the same way whatever track they came
    // from: tempo, then the controllers that shape a note, then note-offs, then
    // note-ons. Sorting on the tick alone leaves them in track order, and two
    // files that carry identical events in a different number of tracks then
    // render differently - which is not a difference between the files. It
    // measured as a correlation of 0.53 between two note-for-note identical
    // renders of BAMBOO.
    auto rank = [](const Ev& e) -> int {
        if (e.isTempo) return 0;
        const int kind = e.status & 0xF0;
        if (kind == 0x80 || (kind == 0x90 && e.d2 == 0)) return 2;
        if (kind == 0x90) return 3;
        return 1;
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const Ev& a, const Ev& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         return rank(a) < rank(b);
                     });
    return !out.empty();
}

void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        printf("usage: midiwav <in.mid|in.rcp> <out.wav> <soundfont.sf2> [seconds]\n");
        return 2;
    }
    const QString inPath  = QString::fromLocal8Bit(argv[1]);
    const QString outPath = QString::fromLocal8Bit(argv[2]);
    const QString sfPath  = QString::fromLocal8Bit(argv[3]);
    double maxSeconds = 0.0;
    double fromSeconds = 0.0;
    int    chaseMode = 0;   // 1 = current algorithm, 2 = the six-controller one
    for (int i = 4; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--from" && i + 1 < argc)       { fromSeconds = atof(argv[++i]); }
        else if (a == "--chase" && i + 1 < argc) { fromSeconds = atof(argv[++i]); chaseMode = 1; }
        else if (a == "--chase-old" && i + 1 < argc) { fromSeconds = atof(argv[++i]); chaseMode = 2; }
        else                                     { maxSeconds = a.toDouble(); }
    }
    const int rate = 44100;

    QFile fin(inPath);
    if (!fin.open(QIODevice::ReadOnly)) { printf("cannot read %s\n", argv[1]); return 1; }
    QByteArray raw = fin.readAll();
    fin.close();

    // A Recomposer file goes through the real converter first.
    //
    // It MUST be the path overload. The QByteArray one cannot look beside the
    // song for the .GSD control file, so it converts without the part setup -
    // no levels, no pan, no rhythm assignment - and every part then plays at
    // the module's defaults. That renders LOUDER than the reference and lines
    // up with nothing, which is exactly what it looked like when this was
    // reported as a conversion fault.
    if (RcpFileHandler::isRcpData(raw)) {
        const QByteArray smf = RcpFileHandler::extractMidiData(inPath);
        if (smf.isEmpty()) { printf("RCP conversion failed\n"); return 1; }
        raw = smf;
    }

    std::vector<Ev> evs;
    int division = 480;
    if (!parseSmf(raw, evs, division)) { printf("not a MIDI file\n"); return 1; }

    tsf* synth = tsf_load_filename(sfPath.toLocal8Bit().constData());
    if (!synth) { printf("cannot load soundfont %s\n", argv[3]); return 1; }
    tsf_set_output(synth, TSF_STEREO_INTERLEAVED, rate, 0);
    tsf_channel_set_presetnumber(synth, 9, 0, 1);      // channel 10 is drums

    std::vector<float> pcm;
    double usPerQuarter = 500000.0;
    double samplesPerTick = (usPerQuarter / division) * rate / 1000000.0;
    uint32_t lastTick = 0;
    double carry = 0.0;
    const size_t frameCap = (maxSeconds > 0.0) ? size_t(maxSeconds * rate) : size_t(-1);

    auto renderTo = [&](uint32_t tick) {
        if (tick < lastTick) return;
        double want = double(tick - lastTick) * samplesPerTick + carry;
        int frames = int(want);
        carry = want - frames;
        lastTick = tick;
        while (frames > 0 && pcm.size() / 2 < frameCap) {
            const int chunk = (frames < 4096) ? frames : 4096;
            const size_t at = pcm.size();
            pcm.resize(at + size_t(chunk) * 2, 0.0f);
            tsf_render_float(synth, pcm.data() + at, chunk, 0);
            frames -= chunk;
        }
    };

    // Where to begin, in ticks, walking the tempo map to get there.
    uint32_t fromTick = 0;
    if (fromSeconds > 0.0) {
        double us = 0.0, upq = 500000.0;
        uint32_t prev = 0;
        const double wantUs = fromSeconds * 1000000.0;
        for (const Ev& e : evs) {
            if (!e.isTempo) continue;
            const double span = double(e.tick - prev) * upq / division;
            if (us + span >= wantUs) break;
            us += span; prev = e.tick; upq = double(e.tempo);
        }
        fromTick = prev + uint32_t((wantUs - us) * division / upq);
    }

    if (fromTick > 0) {
        // Everything before the start point, applied in order but silent. In
        // --from this IS the state; in --chase it is only used to fill the
        // snapshot below.
        unsigned char cc[16][128];
        memset(cc, 0xFF, sizeof(cc));
        int prog[16], bend[16], lastNrpn[16], lastRpn[16];
        for (int c = 0; c < 16; ++c) { prog[c] = 0; bend[c] = 8192; lastNrpn[c] = -1; lastRpn[c] = -1; }

        for (const Ev& e : evs) {
            if (e.tick >= fromTick) break;
            if (e.isTempo) {
                usPerQuarter = double(e.tempo);
                samplesPerTick = (usPerQuarter / division) * rate / 1000000.0;
                continue;
            }
            const int ch = e.status & 0x0F;
            switch (e.status & 0xF0) {
                case 0xB0:
                    if (e.d1 < 128) cc[ch][e.d1] = e.d2;
                    if (e.d1 == 98 || e.d1 == 99) {
                        lastNrpn[ch] = ((cc[ch][99] == 0xFF ? 0 : cc[ch][99]) << 7)
                                     |  (cc[ch][98] == 0xFF ? 0 : cc[ch][98]);
                        lastRpn[ch] = -1;
                    } else if (e.d1 == 100 || e.d1 == 101) {
                        lastRpn[ch] = ((cc[ch][101] == 0xFF ? 0 : cc[ch][101]) << 7)
                                    |  (cc[ch][100] == 0xFF ? 0 : cc[ch][100]);
                        lastNrpn[ch] = -1;
                    }
                    if (chaseMode == 0) tsf_channel_midi_control(synth, ch, e.d1, e.d2);
                    break;
                case 0xC0:
                    prog[ch] = e.d1;
                    if (chaseMode == 0) tsf_channel_set_presetnumber(synth, ch, e.d1, ch == 9);
                    break;
                case 0xE0:
                    bend[ch] = e.d1 | (e.d2 << 7);
                    if (chaseMode == 0) tsf_channel_set_pitchwheel(synth, ch, bend[ch]);
                    break;
                default: break;
            }
        }

        if (chaseMode == 2) {
            // What performSeekImmediate used to restore, and nothing else.
            for (int ch = 0; ch < 16; ++ch) {
                tsf_channel_midi_control(synth, ch, 1,  cc[ch][1]  == 0xFF ? 0   : cc[ch][1]);
                tsf_channel_midi_control(synth, ch, 7,  cc[ch][7]  == 0xFF ? 100 : cc[ch][7]);
                tsf_channel_midi_control(synth, ch, 11, cc[ch][11] == 0xFF ? 127 : cc[ch][11]);
                tsf_channel_midi_control(synth, ch, 64, cc[ch][64] == 0xFF ? 0   : cc[ch][64]);
                tsf_channel_set_presetnumber(synth, ch, prog[ch], ch == 9);
                tsf_channel_set_pitchwheel(synth, ch, bend[ch]);
            }
        } else if (chaseMode == 1) {
            // The snapshot, in MidiPlayer::sendCurrentChannelState's order.
            for (int ch = 0; ch < 16; ++ch) {
                if (cc[ch][0]  != 0xFF) tsf_channel_midi_control(synth, ch, 0,  cc[ch][0]);
                if (cc[ch][32] != 0xFF) tsf_channel_midi_control(synth, ch, 32, cc[ch][32]);
                tsf_channel_set_presetnumber(synth, ch, prog[ch], ch == 9);
                for (int k = 1; k < 128; ++k) {
                    if (k == 6 || k == 32 || k == 38 || k == 98 || k == 99 || k == 100 || k == 101) continue;
                    if (k >= 120) continue;
                    if (cc[ch][k] == 0xFF) continue;
                    tsf_channel_midi_control(synth, ch, k, cc[ch][k]);
                }
                const int addrN = lastNrpn[ch], addrR = lastRpn[ch];
                if (addrN >= 0) {
                    tsf_channel_midi_control(synth, ch, 99, (addrN >> 7) & 0x7F);
                    tsf_channel_midi_control(synth, ch, 98, addrN & 0x7F);
                    if (cc[ch][6]  != 0xFF) tsf_channel_midi_control(synth, ch, 6,  cc[ch][6]);
                    if (cc[ch][38] != 0xFF) tsf_channel_midi_control(synth, ch, 38, cc[ch][38]);
                }
                if (addrR >= 0) {
                    tsf_channel_midi_control(synth, ch, 101, (addrR >> 7) & 0x7F);
                    tsf_channel_midi_control(synth, ch, 100, addrR & 0x7F);
                    if (cc[ch][6]  != 0xFF) tsf_channel_midi_control(synth, ch, 6,  cc[ch][6]);
                    if (cc[ch][38] != 0xFF) tsf_channel_midi_control(synth, ch, 38, cc[ch][38]);
                }
                tsf_channel_set_pitchwheel(synth, ch, bend[ch]);
            }
        }
        lastTick = fromTick;
    }

    for (const Ev& e : evs) {
        if (e.tick < fromTick) continue;
        renderTo(e.tick);
        if (pcm.size() / 2 >= frameCap) break;
        if (e.isTempo) {
            usPerQuarter = double(e.tempo);
            samplesPerTick = (usPerQuarter / division) * rate / 1000000.0;
            continue;
        }
        const int ch = e.status & 0x0F;
        switch (e.status & 0xF0) {
            case 0x90:
                if (e.d2) tsf_channel_note_on(synth, ch, e.d1, e.d2 / 127.0f);
                else      tsf_channel_note_off(synth, ch, e.d1);
                break;
            case 0x80: tsf_channel_note_off(synth, ch, e.d1); break;
            case 0xB0: tsf_channel_midi_control(synth, ch, e.d1, e.d2); break;
            case 0xC0: tsf_channel_set_presetnumber(synth, ch, e.d1, ch == 9); break;
            case 0xE0: tsf_channel_set_pitchwheel(synth, ch, e.d1 | (e.d2 << 7)); break;
            default: break;
        }
    }

    // Let the tails ring.
    for (int i = 0; i < 22 && pcm.size() / 2 < frameCap; ++i) {
        const size_t at = pcm.size();
        pcm.resize(at + 4096 * 2, 0.0f);
        tsf_render_float(synth, pcm.data() + at, 4096, 0);
    }

    FILE* f = fopen(outPath.toLocal8Bit().constData(), "wb");
    if (!f) { printf("cannot write %s\n", argv[2]); return 1; }
    std::vector<int16_t> s16(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        float v = pcm[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        s16[i] = int16_t(v * 32767.0f);
    }
    const uint32_t bytes = uint32_t(s16.size() * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, uint32_t(rate)); put32(f, uint32_t(rate * 4)); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(s16.data(), 1, bytes, f);
    fclose(f);

    printf("%-30s %6.1f s  %zu events  division %d\n",
           QFileInfo(outPath).fileName().toLocal8Bit().constData(),
           double(pcm.size() / 2) / rate, evs.size(), division);
    tsf_close(synth);
    return 0;
}
