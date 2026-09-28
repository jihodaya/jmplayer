// .mdz - the compiled data format of MLD, the X68000 OPM/ADPCM/MIDI driver by
// LUM2 (1990-93). Tracks routed to a MIDI module are turned into a Standard
// MIDI File here and the ordinary MIDI path plays them.
//
// EVERY RULE BELOW WAS MEASURED, NOT REASONED. The driver package survives on
// the NFG Games mirror of x68pub (SOUND/MLD/), and it ships mlc.x - the MML
// compiler that produced these files - and mdz2mus.x, which turns one back
// into MML. Both are Human68k console programs and run68 (kg68k/run68x) runs
// them on Windows with no ROM and no operating system. So the format has an
// answer key: write MML, compile it, read the bytes.
//
//     A C1 V105 @v115 p64 @16 @t228 y91,40 o4 c4 r4 d8 e8
//     E1 10  E3 16  FB 8C  FC 40  FD 10  FF E4  FE 5B 28  3C 30  80 30 ...
//
// That one line settles the note encoding, the tick base and six commands.
//
// THIS IS NOT MDX, and the earlier port's worst mistake came from assuming it
// was. MLD's own documentation calls itself "MXDRV + MDD ライクな" - MDX-LIKE -
// so the header and offset table match while the commands do not. Giving
// 0xE0-0xFF the operand counts from MDX's table was tried on 2026-09-02 and the
// owner heard it as much worse; it is reverted and must not be tried again.
//
// The port it replaces was a copy of a working-but-heuristic Python player.
// What that player had wrong, all of it now measured:
//
//   * velocity (0xFB) was SKIPPED as an unknown one-operand command, and a
//     blend of two other values used in its place;
//   * volume (0xE3) and velocity are ATTENUATIONS - V = 127 - n, @v = 255 - n -
//     so reading them straight inverts loud and quiet;
//   * 0xD6 was read as velocity with one operand. It is neither: over 1,829
//     uses not one operand is a value from the documented v-to-@v table;
//   * 0xF6/0xF5 are a REAL LOOP with a signed relative jump, where the player
//     invented a "pattern repeat" that replayed the last few notes;
//   * the tempo command 0xFF was never read at all - the BPM came from
//     keywords in the FILENAME;
//   * low notes were remapped into the General MIDI kit. Notes are raw MIDI
//     note numbers (c4 compiles to 0x3C = 60), so nothing needs remapping;
//   * reverb, chorus, sustain and expression were injected at tick 0. The
//     songs set their own - AB2_05g asks for reverb 40 and chorus 0, and was
//     being given 65 and 30.

#include "mdxmidi.h"

#include <QFile>
#include <QFileInfo>
#include <vector>
#include <algorithm>
#include <string>
#include <windows.h>

namespace {

struct Ev {
    unsigned long tick;
    unsigned char status, d1, d2;
    int order;                       // keeps equal ticks in stream order
    QByteArray blob;                 // SysEx payload, when status is 0xF0
};

// How many operand bytes each command takes. Every entry was obtained by
// compiling the MML on the right with mlc.x and reading what came out.
//
//   9A BS~/BS_      9B @F fade     9D @BS         9F VC restore
//   A0 @vC restore  A1 tC restore  A4 @t+/-       A7 BOF
//   A9 MA lfo       AA MP 4-arg    AB MP lfo      AC @_ portamento
//   AD _ portamento AE MZS         B1 p-          B2 p+
//   B3 D~/D_        B4 D detune    B9 #SC:INIT    BA #CM64:INIT
//   C2 V-           C3 V+          C5 R marker    C6 J
//   B0 MZ vol lfo    C4 @_ porta    C7 APOF        C8 APON
//   D0 DV device    D4 RA          D6 NRPN        D9 BS
//   DA BR           DB MOF         DD ?           DF O
//   E0 RT           E1 C channel   E2 P sustain   E3 V volume
//   E4 K/T          E6 ` key off   E7 ?           E8 MPON/MAON
//   E9 M modulate   EA MH, 5       EC lfo, 5      ED F pcm rate
//   EE W wait
//   EF S sync
//   F0 k delay      F7 & tie       F8 q/@q gate   F9 ( vel down
//   FA ) vel up     FB v/@v        FC p pan       FD @ program
//   FE y control    FF t/@t tempo
//
// 0xCD and 0xD8 are the two whose MML no probe reached; their length is fitted
// against the compiler's own clock counts, which is also what says a length is
// right for the rest.
//
// 0xEC was fitted too, at 3, and that was wrong: it is 5. Ten tracks in three
// songs (ken_pcm, ADYRMSC, MHGS_BW) carry it, and in every one the byte after
// the fifth operand is E9, E8 or 80 - a modulation command, MPON, a rest -
// where the byte after the third is 0x00-0x05, a note below the audible range.
// ken_pcm's first OPM track reads `EC 00 00 0C 00 80 E8 02`: at 3 that is a
// note 0 held for 0x80 ticks, which pushed the whole track 128 ticks - 0.88 s -
// late against its own MIDI half. Reported as the two halves playing apart. A
// fit against tick totals cannot catch this, because a linear walk resyncs on
// the next command either way (2026-09-14).
//
// Two more from the same song and the same day. 0xED is MDX's F - the ADPCM
// rate, one operand - and it sits at the head of both of ken_pcm's PCM tracks
// as `ED 04`; consumed with no operand, the 04 became a note and the byte
// after it a length of 226 ticks, which played one sample at full scale at
// tick 0 and put both drum tracks 1.56 s late. ADT00SC opens its PCM track the
// same way. 0xC1 takes two operands: MHGS_BW uses it 136 times and at two the
// byte that follows is always a note between 74 and 82, at three always a
// length - it was landing inside its own operands and playing them.
//
// 0xEA is `MH`, the OPM's hardware LFO, and it takes FIVE (2026-09-28,
// compiled: `MH2,207,4,0,0,0,0` is `EA 02 CF 84 00 00`, `MH3,255,127,127,7,3,1`
// is `EA 43 FF FF 7F 73` - MXDRV's long form, byte for byte). It was listed as
// unknown, so `02 CF` became a note held 207 clocks: every FM track of the
// GRADIUS II set ran 1983 clocks where the compiler counts 1776, and its OPM
// half slid off the MIDI half it doubles. `MHON`/`MHOF` are `E8 10`/`E8 00`.
//
// 0xD7 (EX) and 0xCA (WEX) are exclusives and run to an 0xF7 terminator, so
// they are length-counted separately. 0xF1, 0xF4, 0xF5 and 0xF6 carry jumps
// and are handled in the walk.
//
// Anything not listed is consumed with no operands. Over the 176 files here
// that now leaves 216 commands of 15 kinds unaccounted for - excluding
// ANGE10, whose 16,001 were a runaway loop rather than a command - and
// **94.5% of tracks match the tick count the compiler reports for them**,
// against 65.9% when this table was first assembled.
const signed char kOperands[128] = {
//        0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F
/*80*/   -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
/*90*/   -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  1,  2, -1,  2, -1,  0,
/*A0*/    0,  0, -1, -1,  2, -1, -1,  0, -1,  5,  7,  5,  2,  2,  1, -1,
/*B0*/    5,  1,  1,  2,  2, -1, -1, -1, -1,  0,  0, -1, -1, -1, -1, -1,
/*C0*/   -1,  2,  1,  1,  2,  0,  1,  0,  0,  3, -1,  1, -1,  1, -1, -1,
/*D0*/    1, -1, -1, -1,  0, -1,  3, -1,  1,  1,  1,  0, -1,  2, -1,  1,
/*E0*/    0,  1,  1,  1,  1, -1,  0,  1,  1,  2,  5, -1,  5,  1,  0,  1,
/*F0*/    1, -1, -1, -1, -1, -1, -1,  0,  1,  1,  1,  1,  1,  1,  2,  1,
};

// Commands whose operands run to an 0xF7 terminator rather than a fixed count
// are handled in the walk: 0xD7 / 0xCA are the MML EX / WEX exclusives and
// 0xBD / 0xBE / 0xBF the module macros, all of which go out as SysEx, while
// 0xB7 and 0xBB are consumed without being forwarded.
// The documented v# -> @v# curve for a MIDI part. mlc.x emits the coarse form
// with bit 7 clear, so the low nibble indexes this.
const unsigned char kVelCurve[16] = {
    0, 7, 15, 23, 31, 39, 47, 55, 64, 71, 79, 87, 95, 103, 111, 119,
};

// Title, 0D 0A 1A, PDX name, NUL, then a big-endian offset table: the voice
// offset followed by one per track. A few files sit some bytes further on, so
// the base is walked forward until the table it describes is coherent - and the
// entries are 16-bit in most songs and 32-bit in some, with nothing in the file
// to say which, so both widths are tried.
bool header(const unsigned char* d, int n, int& base, std::vector<int>& offs,
            int* voiceOut = nullptr)
{
    int i = 0;
    while (i < n) {
        if (d[i] == 0x0D && i + 2 < n && d[i + 1] == 0x0A && d[i + 2] == 0x1A) { i += 3; break; }
        if (d[i] == 0x1A) { ++i; break; }
        ++i;
    }
    while (i < n && d[i] != 0x00) ++i;
    if (i < n) ++i;

    // The common table: 16-bit big-endian, the voice table then one offset per
    // track, all relative to the table's own start.
    for (int adj = 0; adj <= 32; adj += 2) {
        const int b = i + adj;
        if (b + 4 > n) break;
        const int first = (d[b + 2] << 8) | d[b + 3];
        if (first < 4 || (first & 1) || b + first > n) continue;
        const int nt = (first - 2) / 2;
        if (nt < 1 || nt > 32) continue;
        const int voice = (d[b] << 8) | d[b + 1];
        bool ok = (voice >= first);
        std::vector<int> o;
        for (int t = 0; t < nt && ok; ++t) {
            const int v = (d[b + 2 + t * 2] << 8) | d[b + 3 + t * 2];
            if (v != 0 && v < first) ok = false;
            o.push_back(v);
        }
        if (ok) { base = b; offs = o; if (voiceOut) *voiceOut = voice; return true; }
    }

    // Some songs write the same table 32-bit instead, and nothing in the file
    // says which - the entries are simply four bytes with a zero high word.
    // Measured: 18 of the 176 songs in the older collection and all 7 of the
    // GRADIUS II set, and every one of them found no tracks at all and played
    // silence until this was added.
    for (int adj = 0; adj <= 32; adj += 2) {
        const int b = i + adj;
        if (b + 8 > n) break;
        if (d[b] || d[b + 1] || d[b + 4] || d[b + 5]) continue;   // high words
        const int first = (d[b + 6] << 8) | d[b + 7];
        if (first < 8 || (first & 3) || b + first > n) continue;
        const int nt = (first - 4) / 4;
        if (nt < 1 || nt > 32) continue;
        const int voice = (d[b + 2] << 8) | d[b + 3];
        bool ok = (voice >= first);
        std::vector<int> o;
        for (int t = 0; t < nt && ok; ++t) {
            const int e = b + 4 + t * 4;
            if (d[e] || d[e + 1]) { ok = false; break; }
            const int v = (d[e + 2] << 8) | d[e + 3];
            if (v != 0 && v < first) ok = false;
            o.push_back(v);
        }
        if (ok) { base = b; offs = o; if (voiceOut) *voiceOut = voice; return true; }
    }
    return false;
}

// The title is Shift-JIS, always - these are X68000 files.
QString titleOf(const unsigned char* d, int n)
{
    int end = -1;
    for (int i = 0; i + 2 < n; ++i)
        if (d[i] == 0x0D && d[i + 1] == 0x0A && d[i + 2] == 0x1A) { end = i; break; }
    if (end < 0)
        for (int i = 0; i < n; ++i)
            if (d[i] == 0x1A) { end = i; break; }
    if (end <= 0) return QString();

    const int len = MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(d), end,
                                        nullptr, 0);
    if (len <= 0) return QString();
    std::wstring buf(size_t(len), L'\0');
    if (MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(d), end,
                            &buf[0], len) <= 0)
        return QString();
    return QString::fromWCharArray(buf.data(), len).trimmed();
}

void pushVar(QByteArray& out, quint32 v)
{
    quint32 buf = v & 0x7F;
    while ((v >>= 7)) { buf <<= 8; buf |= 0x80; buf += (v & 0x7F); }
    for (;;) { out.append(char(buf & 0xFF)); if (buf & 0x80) buf >>= 8; else break; }
}

void pushBE32(QByteArray& out, quint32 v)
{
    out.append(char((v >> 24) & 0xFF)); out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));  out.append(char(v & 0xFF));
}

int signed16(const unsigned char* d, int n, int p)
{
    if (p + 1 >= n) return 0;
    const int v = (d[p] << 8) | d[p + 1];
    return (v & 0x8000) ? (v - 0x10000) : v;
}

struct Walker {
    const unsigned char* d;
    int n;
    std::vector<Ev>* out;
    int order = 0;

    void put(unsigned long tick, int status, int d1, int d2)
    {
        Ev e;
        e.tick = tick;
        e.status = (unsigned char)status;
        e.d1 = (unsigned char)d1;
        e.d2 = (unsigned char)d2;
        e.order = order++;
        out->push_back(e);
    }
    void putBlob(unsigned long tick, const QByteArray& b)
    {
        Ev e;
        e.tick = tick; e.status = 0xF0; e.d1 = 0; e.d2 = 0;
        e.order = order++; e.blob = b;
        out->push_back(e);
    }

    // A track that names an OPM channel is still walked - the tempo command is
    // GLOBAL on this driver and seven of the twelve mixed songs here keep it in
    // one half only - but nothing it plays belongs in the SMF.
    void track(int start, int end, bool audible)
    {
        int ch = 0;
        unsigned long tick = 0;
        int vel = 100;                       // MLD leaves this undefined
        int gateQ = 0;                       // 0 = none; else the 0xF8 operand
        int dev = 0x10;                      // DV17, the default the doc names
        bool tie = false;
        int lastNote = -1;
        size_t lastOffIndex = size_t(-1);

        std::vector<int> loop;               // plays left, per open loop
        int p = start;
        long steps = 0;
        // A jump can close a cycle, and one file here does exactly that:
        // ANGE10 is 2,399 bytes with six 0xCD in it, and an unknown command
        // read at the wrong length sent the walk round a loop 16,001 times.
        // A flat two-million cap let that produce a huge SMF; bounding the
        // work by the track's own size stops it while leaving room for the
        // repeats a song legitimately writes.
        const long kMaxSteps = long(end - start) * 64 + 4096;

        while (p >= start && p < end && p < n && ++steps < kMaxSteps) {
            const int opPos = p;
            const unsigned char c = d[p++];

            if (c < 0x80) {                                  // note + duration
                if (p >= end) break;
                const int dur = d[p++];
                // q# is how many eighths of the note actually sound; @q# cuts
                // a fixed number of clocks off the end and is stored as
                // 255 - n (q1..q8 compile to F8 01..08, @q0/@q8/@q192 to
                // F8 FF / F8 F7 / F8 3F). With neither, one clock of gap keeps
                // successive notes of the same pitch apart.
                int gate;
                if (gateQ == 0)            gate = (dur > 1) ? (dur - 1) : dur;
                else if (gateQ & 0x80)     gate = dur - (255 - gateQ);
                else                       gate = dur * gateQ / 8;
                if (gate < 1) gate = 1;
                if (!audible) {
                    tick += (unsigned long)dur;
                    lastNote = c; tie = false;
                    continue;
                }
                if (tie && lastNote == int(c) && lastOffIndex != size_t(-1)) {
                    // & joins the note already sounding: move its release out
                    // rather than striking it again.
                    (*out)[lastOffIndex].tick = tick + (unsigned long)gate;
                } else {
                    put(tick, 0x90 | ch, c, vel);
                    put(tick + (unsigned long)gate, 0x80 | ch, c, 0);
                    lastOffIndex = out->size() - 1;
                }
                lastNote = c;
                tie = false;
                tick += (unsigned long)dur;
                continue;
            }
            if (c == 0x80) {                                 // rest + duration
                if (p >= end) break;
                tick += d[p++];
                lastNote = -1;
                tie = false;
                continue;
            }

            switch (c) {
            case 0xE1:                                       // C / CH / CHP - module
                // Bit 4 set is a MIDI channel (C#), clear an OPM or PCM one
                // (CH#, CHP#). The walk follows it rather than trusting the
                // pre-scan in toMidi(), which only looks 64 bytes in.
                if (p < end) { const int a = d[p++]; ch = a & 0x0F; audible = (a & 0x10) != 0; }
                break;
            case 0xFD:                                       // @ - program
                if (p < end) { const int v = d[p++] & 0x7F; if (audible) put(tick, 0xC0 | ch, v, 0); }
                break;
            case 0xFC:                                       // p - pan
                if (p < end) { const int v = d[p++] & 0x7F; if (audible) put(tick, 0xB0 | ch, 10, v); }
                break;
            case 0xE2:                                       // P - sustain
                if (p < end) { const int v = d[p++] & 0x7F; if (audible) put(tick, 0xB0 | ch, 64, v); }
                break;
            case 0xE3:                                       // V - main volume
                if (p < end) {
                    const int a = d[p++];
                    if (audible) put(tick, 0xB0 | ch, 7, (a > 127) ? 0 : (127 - a));
                }
                break;
            case 0xC3:                                       // V+
            case 0xC2:                                       // V-
                if (p < end) ++p;                            // relative, not tracked
                break;
            case 0xFE:                                       // y - control change
                if (p + 1 < end) {
                    if (audible) put(tick, 0xB0 | ch, d[p] & 0x7F, d[p + 1] & 0x7F);
                    p += 2;
                } else p = end;
                break;
            case 0xFB:                                       // v / @v - velocity
                if (p < end) {
                    const int a = d[p++];
                    vel = (a & 0x80) ? (255 - a) : kVelCurve[a & 0x0F];
                    if (vel < 1) vel = 1;
                    if (vel > 127) vel = 127;
                }
                break;
            case 0xFA:                                       // ) - velocity up
                if (p < end) { vel += d[p++]; if (vel > 127) vel = 127; }
                break;
            case 0xF9:                                       // ( - velocity down
                if (p < end) { vel -= d[p++]; if (vel < 1) vel = 1; }
                break;
            case 0xFF:                                       // t / @t - tempo
                if (p < end) {
                    const int b = d[p++];
                    // The operand is the OPM's timer B, and a larger value is
                    // faster. 48 ticks to a quarter note, which the compiler
                    // confirms: r4 is 0x30.
                    const int denom = 256 - b;
                    if (denom > 0) {
                        const double bpm = 4882.8125 / double(denom);
                        put(tick, 0xFF, 0x51, 0);           // filled in later
                        out->back().blob = QByteArray()
                            .append(char((int(60000000.0 / bpm) >> 16) & 0xFF))
                            .append(char((int(60000000.0 / bpm) >> 8) & 0xFF))
                            .append(char(int(60000000.0 / bpm) & 0xFF));
                    }
                }
                break;
            case 0xD6:                                       // an NRPN group
                // Three operands, sent as CC99 / CC98 / CC6. mlc.x compiles
                // #SC:CF10 - the SC-55 cutoff macro - to D6 01 20 0A, which is
                // NRPN (1,32) = 10, and mdz2mus decompiles this song's
                // D6 18 2A 40 back to "y99,24 y98,42 y6,64". Both readings
                // agree, and the file contains no plain FE 63 at all, so every
                // parameter these songs set arrives through here.
                //
                // The port this replaces read 0xD6 as a one-operand velocity
                // command: it emitted a wrong value AND left two bytes behind,
                // which the walk then played as a note. 90 of them in this
                // song alone.
                if (p + 2 < end) {
                    if (audible) {
                        put(tick, 0xB0 | ch, 99, d[p] & 0x7F);
                        put(tick, 0xB0 | ch, 98, d[p + 1] & 0x7F);
                        put(tick, 0xB0 | ch,  6, d[p + 2] & 0x7F);
                    }
                    p += 3;
                } else p = end;
                break;
            case 0xF7:                                       // & - tie
                tie = true;
                break;
            case 0xD7:                                       // EX  - exclusive
            case 0xCA: {                                     // WEX - exclusive
                QByteArray b;
                while (p < end && p < n && d[p] != 0xF7) b.append(char(d[p++]));
                if (p < end) ++p;                            // the F7 terminator
                // Exclusives go to the MIDI port whatever module the track
                // drives - an OPM or PCM part has nothing else to send them to,
                // and songs use exactly that. NEW_Wa's first track is its PCM
                // part (CHP1), and before its first sample it uploads five
                // CM-32L timbres to timbre memory (08 00 00 ...), resets the
                // module and sets the partial reserve. Gated on `audible`, all
                // of it was dropped and the nine MIDI parts - set to memory
                // timbres - played nothing at all (2026-09-28).
                if (!b.isEmpty()) {
                    QByteArray sysex;
                    sysex.append(char(0xF0));
                    sysex.append(b);
                    sysex.append(char(0xF7));
                    putBlob(tick, sysex);
                }
                break;
            }
            case 0xF8:                                       // q / @q - gate
                if (p < end) gateQ = d[p++];
                break;
            case 0xD0:                                       // DV - device ID
                if (p < end) dev = d[p++] & 0x7F;
                break;
            case 0xBD: case 0xBE: case 0xBF: {
                // A module macro, and the stored bytes are the Roland message
                // from the address onwards - CHECKSUM INCLUDED, which is what
                // proves the layout: #SC:EM1 compiles to BE 40 01 30 01 0E F7,
                // and 0x40+0x01+0x30+0x01 = 0x72 with 128 - 0x72 = 0x0E. The
                // driver supplies only the F0 41 <dev> <model> 12 header, so it
                // can be rebuilt exactly rather than guessed at.
                const int model = (c == 0xBE) ? 0x42     // GS      - SC-55
                                : (c == 0xBF) ? 0x16     // LA      - MT-32/CM-64
                                              : 0x45;    // SC-55 display
                QByteArray b;
                while (p < end && p < n && d[p] != 0xF7) b.append(char(d[p++]));
                if (p < end) ++p;
                if (!b.isEmpty()) {                          // any track - see EX
                    QByteArray sysex;
                    sysex.append(char(0xF0));
                    sysex.append(char(0x41));
                    sysex.append(char(dev));
                    sysex.append(char(model));
                    sysex.append(char(0x12));
                    sysex.append(b);
                    sysex.append(char(0xF7));
                    putBlob(tick, sysex);
                }
                break;
            }
            case 0xB7: case 0xBB:
                // U-110 and one macro whose model byte is not established.
                // Consumed, not forwarded - a wrong model byte would be worse
                // than silence.
                while (p < end && p < n && d[p] != 0xF7) ++p;
                if (p < end) ++p;
                break;
            case 0xF6:                                       // [ - loop start
                if (p + 1 < end) loop.push_back(d[p]);
                p += 2;
                break;
            case 0xF4: {                                     // / - loop escape
                const int off = signed16(d, n, p);
                p += 2;
                // Only the LAST pass skips ahead. The target is measured from
                // the second operand byte, unlike the loop end below.
                if (!loop.empty() && loop.back() <= 1) {
                    const int q = opPos + 2 + off;
                    if (q >= start && q < end) p = q;
                }
                break;
            }
            case 0xF5: {                                     // ] - loop end
                const int off = signed16(d, n, p);
                p += 2;
                if (!loop.empty()) {
                    if (--loop.back() >= 1) {
                        const int q = p + off;
                        if (q >= start && q < end) { p = q; continue; }
                        p = end;
                    } else {
                        loop.pop_back();
                    }
                }
                break;
            }
            case 0xF1:
                // 0xF1 00 ends the track; anything else is an infinite loop
                // back to a byte in the track. A Standard MIDI File is finite,
                // so it plays once - the same reading Recomposer's step-0 loop
                // marker gets.
                p = end;
                break;
            default: {
                const int k = kOperands[c - 0x80];
                p += (k > 0) ? k : 0;
                break;
            }
            }
        }
    }
};

} // namespace

namespace mdxmidi {

bool parseHeader(const unsigned char* d, int n, int& base,
                 std::vector<int>& trackOffsets, int& voiceOffset)
{
    // The voice offset comes back from the table walk rather than being read
    // at `base` here: a 32-bit table keeps it two bytes further in, and reading
    // it blind gave zero on every one of those songs.
    return header(d, n, base, trackOffsets, &voiceOffset);
}

int operandCount(unsigned char c)
{
    if (c < 0x80) return 1;                    // a note carries its duration
    return kOperands[c - 0x80];
}

int firstChannelOperand(const unsigned char* d, int n, int start, int end)
{
    // The old rule was "an 0xE1 in the first 64 bytes", and that window is
    // kept - it just no longer counts the inside of an exclusive. Operand
    // bytes cannot be told from notes without walking the commands, so the
    // scan does not stop at the first byte under 0x80; stopping there
    // misread `FC 03 E1 00` as "no channel" and lost 19 of 27 FM songs.
    const int stop = std::min(end, n);
    int counted = 0;
    for (int i = start; i + 1 < stop && counted < 64; ) {
        const unsigned char c = d[i];
        if (c == 0xE1) return d[i + 1];
        if (isBlobCommand(c)) {                     // skip to past the F7
            ++i;
            while (i < stop && d[i] != 0xF7) ++i;
            ++i;
            continue;
        }
        ++i; ++counted;
    }
    return -1;
}

bool isBlobCommand(unsigned char c)
{
    return c == 0xD7 || c == 0xCA || c == 0xB7
        || c == 0xBB || c == 0xBD || c == 0xBE || c == 0xBF;
}

int velocityCurve(int index)
{
    return kVelCurve[index & 0x0F];
}


bool hasMidiTracks(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray raw = f.readAll();
    f.close();
    if (raw.size() < 32) return false;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(raw.constData());
    const int n = raw.size();

    int base = 0; std::vector<int> offs;
    if (!header(d, n, base, offs)) return false;

    // A MIDI track opens by naming its channel with 0xE1, whose operand is
    // 0x10 | (channel - 1) - C1 compiles to E1 10 and C10 to E1 19. Only the
    // first bytes of each track are looked at, so an 0xE1 that is really some
    // other command's operand cannot claim the file: this answer decides
    // whether a song goes to the chip engine or the MIDI one.
    for (size_t t = 0; t < offs.size(); ++t) {
        if (offs[t] == 0) continue;
        const int st = base + offs[t];
        const int en = (t + 1 < offs.size() && offs[t + 1]) ? base + offs[t + 1] : n;
        const int stop = (en < st + 64) ? en : st + 64;
        for (int i = st; i + 1 < stop && i + 1 < n; ++i)
            if (d[i] == 0xE1 && (d[i + 1] & 0xF0) == 0x10) return true;
    }
    return false;
}

QByteArray toMidi(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    const QByteArray raw = f.readAll();
    f.close();
    if (raw.size() < 32) return QByteArray();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(raw.constData());
    const int n = raw.size();

    int base = 0; std::vector<int> offs;
    if (!header(d, n, base, offs)) return QByteArray();

    std::vector<int> starts;
    for (size_t t = 0; t < offs.size(); ++t)
        if (offs[t]) starts.push_back(base + offs[t]);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    if (starts.empty()) return QByteArray();

    std::vector<Ev> ev;
    Walker w{d, n, &ev, 0};
    for (size_t k = 0; k < starts.size(); ++k) {
        const int st = starts[k];
        const int en = (k + 1 < starts.size()) ? starts[k + 1] : n;
        // Only the tracks that name a MIDI channel play. The rest belong to
        // the OPM and are walked for their TEMPO alone - `@t` is global on this
        // driver, and seven of the twelve mixed songs keep it in one half only,
        // so a half that ignores the other runs at the wrong speed. ADMADSC
        // holds `@t218` (128.5 BPM) in its MIDI tracks and nothing in its FM
        // ones; the FM engine defaulted to 87.2 and the two drifted apart
        // within a bar, which is what "they are playing separately" was.
        const int a = mdxmidi::firstChannelOperand(d, n, st, en);
        const bool audible = (a >= 0) && (a & 0x10);
        if (st < en) w.track(st, en, audible);
    }
    if (ev.empty()) return QByteArray();

    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;
    });

    const int division = 48;                  // r4 compiles to 0x30 = 48
    QByteArray trk;

    const QString title = titleOf(d, n);
    if (!title.isEmpty()) {
        const QByteArray utf8 = title.toUtf8();
        if (utf8.size() <= 127) {
            // UTF-8 because MidiPlayer::decodeMetaText tries it first and
            // strictly; raw Shift-JIS would be claimed by CP949.
            for (int type = 0x03; type >= 0x01; type -= 0x02) {
                trk.append(char(0x00));
                trk.append(char(0xFF));
                trk.append(char(type));
                trk.append(char(utf8.size()));
                trk.append(utf8);
            }
        }
    }

    // The driver's own tempo until the song says otherwise: mld.x initialises
    // timer B to 0xC8 (`move.b #$C8,$BA(a5)`, at three places in the
    // resident code - the same 200 MXDRV uses), which is 87.2 BPM at 48 ticks
    // to the quarter. Without this the SMF default of 120 applied, so any
    // stretch before a song's first @t ran 38% fast on the MIDI half only -
    // the FM half already starts at 200. The GRADIUS II set opens with 48
    // ticks of rest before its @t, and its MIDI part came in 0.174 s ahead of
    // the OPM doubling it, for the whole song (2026-09-28).
    {
        const int us = int(60000000.0 / (4882.8125 / double(256 - 0xC8)));
        trk.append(char(0x00));
        trk.append(char(0xFF)); trk.append(char(0x51)); trk.append(char(0x03));
        trk.append(char((us >> 16) & 0xFF));
        trk.append(char((us >> 8) & 0xFF));
        trk.append(char(us & 0xFF));
    }

    unsigned long last = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
        pushVar(trk, quint32(ev[i].tick - last));
        last = ev[i].tick;
        if (ev[i].status == 0xFF) {                          // tempo
            trk.append(char(0xFF)); trk.append(char(0x51)); trk.append(char(0x03));
            trk.append(ev[i].blob);
        } else if (ev[i].status == 0xF0) {                   // exclusive
            trk.append(char(0xF0));
            pushVar(trk, quint32(ev[i].blob.size() - 1));
            trk.append(ev[i].blob.mid(1));
        } else {
            trk.append(char(ev[i].status));
            trk.append(char(ev[i].d1));
            if ((ev[i].status & 0xF0) != 0xC0 && (ev[i].status & 0xF0) != 0xD0)
                trk.append(char(ev[i].d2));
        }
    }
    trk.append(char(0x00));
    trk.append(char(0xFF)); trk.append(char(0x2F)); trk.append(char(0x00));

    QByteArray out;
    out.append("MThd");
    pushBE32(out, 6);
    out.append(char(0x00)); out.append(char(0x00));
    out.append(char(0x00)); out.append(char(0x01));
    out.append(char((division >> 8) & 0xFF)); out.append(char(division & 0xFF));
    out.append("MTrk");
    pushBE32(out, quint32(trk.size()));
    out.append(trk);
    return out;
}

} // namespace mdxmidi
