#include "mldfm.h"
#include <cstdio>
#include <cstdlib>
#include "mdxmidi.h"

#include <cstring>
#include <algorithm>

namespace {

// Which of the chip's four slots each algorithm uses as a carrier. Volume is an
// attenuation added to the carriers' total level, so this decides where it goes.
// Same table the MXDRV engine uses, and the same chip.
const uint8_t kCarriers[8] = { 0x08, 0x08, 0x08, 0x08, 0x0C, 0x0E, 0x0E, 0x0F };

// v# -> @v# for an OPM or ADPCM part, and it is NOT the curve a MIDI part uses.
// The manual prints both tables and they share no value: a MIDI part runs
// 0, 7, 15 ... 119 from silence upward, an OPM part 85 ... 125 over a much
// narrower span. The default is v8, so an OPM track that never sets its own
// volume starts at 106 rather than at nothing.
const uint8_t kOpmVelCurve[16] = {
    85, 87, 90, 93, 95, 98, 101, 103, 106, 109, 111, 114, 117, 119, 122, 125,
};

// Semitone to the OPM's key-code field. The chip numbers twelve semitones over
// sixteen values and leaves 3, 7, 11 and 15 unused.
const uint8_t kNoteField[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

// A tick in output samples - EXACT, not rounded. This returned an int, and
// the fraction it threw away is a tempo error: at 49716 Hz and timer B 229
// the tick is 343.6 samples, 343 runs 0.18% fast, and this half of the song
// drifted 130 ms ahead of its MIDI half on every pass. The MIDI half keeps
// wall-clock time from the same timer-B value, so the two have to agree to
// the sample; render16() carries the fraction from tick to tick.
double samplesPerTickFor(int rate, int timerB)
{
    const double period = double(rate) * double(256 - timerB) * 1024.0;
    const double n = period / 4000000.0;
    return (n < 1.0) ? 1.0 : n;
}

} // namespace

MldFm::MldFm() { init(44100); }

void MldFm::init(int sampleRate)
{
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    m_opm.init(4000000, m_sampleRate);
    for (int v = 0; v < kPcmVoices; ++v) m_pcm[v].init(m_sampleRate);
    m_pcmBus.init(m_sampleRate);
    reset();
}

void MldFm::reset()
{
    m_playing = false;
    m_tickCounter = 0;
    m_tickAcc = 0.0;
    m_keyOns = 0;
    m_samplePos = 0;
    m_timerB = 200;
    m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
    m_opm.reset();
    for (int v = 0; v < kPcmVoices; ++v) m_pcm[v].reset();
    m_pcmBus.reset();
    setupTracks();
}

int MldFm::signed16At(int p) const
{
    const int n = int(m_data.size());
    if (p + 1 >= n) return 0;
    const int v = (m_data[p] << 8) | m_data[p + 1];
    return (v & 0x8000) ? (v - 0x10000) : v;
}

bool MldFm::load(const uint8_t* data, size_t size, const uint8_t* pdx, size_t pdxSize)
{
    if (!data || size < 32) return false;
    m_data.assign(data, data + size);
    m_pdx.clear();
    if (pdx && pdxSize) m_pdx.assign(pdx, pdx + pdxSize);

    const unsigned char* d = m_data.data();
    const int n = int(m_data.size());
    std::vector<int> offs;
    m_base = 0; m_voiceOffset = 0;
    if (!mdxmidi::parseHeader(d, n, m_base, offs, m_voiceOffset)) return false;

    // Track boundaries come from the offset table, sorted, because a track runs
    // until the next one begins.
    std::vector<int> starts;
    for (size_t k = 0; k < offs.size(); ++k)
        if (offs[k]) starts.push_back(m_base + offs[k]);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    m_tracks.clear();
    int audibleCount = 0;
    for (size_t k = 0; k < offs.size(); ++k) {
        if (!offs[k]) continue;
        const int st = m_base + offs[k];
        std::vector<int>::iterator it = std::lower_bound(starts.begin(), starts.end(), st);
        const size_t idx = size_t(it - starts.begin());
        const int en = (idx + 1 < starts.size()) ? starts[idx + 1] : n;
        if (st >= en) continue;

        // An OPM track opens with 0xE1 whose operand has bit 4 CLEAR; a MIDI
        // one sets it. Anything that names no channel in its first bytes is
        // left alone rather than guessed at.
        // Every track is kept, because `@t` is GLOBAL on this driver and seven
        // of the twelve mixed songs here write it in their MIDI half only. A
        // track that names a MIDI channel is walked for its tempo and its
        // timing and makes no sound; one that names an OPM channel plays.
        const int a = mdxmidi::firstChannelOperand(d, n, st, en);
        const bool audible = (a >= 0) && !(a & 0x10);
        const int ch = audible ? (a & 0x1F) : -1;

        Track t;
        t.start = st; t.end = en; t.ch = ch; t.audible = audible;
        m_tracks.push_back(t);
        if (audible) ++audibleCount;
    }
    if (audibleCount == 0) return false;

    reset();
    return true;
}

void MldFm::setupTracks()
{
    for (size_t k = 0; k < m_tracks.size(); ++k) {
        Track& t = m_tracks[k];
        t.p = t.start;
        t.wait = 0;
        t.vel = 106;          // v8
        t.velByte = 0x08;
        t.vol = 127;
        t.voiceVol = 0;
        t.gateQ = 0;
        t.gateLeft = -1;
        t.voice = -1;
        t.pan = 3;
        t.tie = false;
        t.active = true;
        t.loop.clear();
        t.ticks = 0;
        t.pmsAms = 0;
        t.slotMask = 0x0F;
        t.alg = 0;
        t.fbAlg = 0;
        for (int i = 0; i < 4; ++i) t.opTL[i] = 0;
    }
}

// A voice record is 34 bytes and opens with its own number - the same rule the
// MXDRV engine had to learn, because only some files number them 0, 1, 2 with
// no gaps.
//
// Layout, measured by compiling a voice whose every parameter differs and
// reading it back:
//
//   +0        voice number
//   +1        (FB << 3) | CON        the chip's 0x20 register, minus the pan
//   +2        slot mask
//   +3        VOL
//   +4..27    the operator parameters, PARAMETER-major: four DT1/MUL, then
//             four TL, KS/AR, AMS/D1R, DT2/D2R, D1L/RR
//   +28..33   WF, CLC, DPS, DLY - the voice's own pitch LFO
//
// MLD stores those four in the chip's own slot order (M1, M2, C1, C2), so they
// go straight through; MXDRV's records are in MML order and need permuting.
const uint8_t* MldFm::findVoice(int number) const
{
    if (m_voiceOffset <= 0) return nullptr;
    const int n = int(m_data.size());
    for (int v = m_base + m_voiceOffset; v + 34 <= n; v += 34) {
        if (m_data[v] == number) return m_data.data() + v;
    }
    return nullptr;
}

void MldFm::sendVoice(Track& t, int voiceIdx)
{
    if (t.ch < 0 || t.ch >= 8) return;
    const uint8_t* v = findVoice(voiceIdx);
    if (!v) return;

    t.fbAlg = v[1] & 0x3F;
    t.alg = v[1] & 0x07;
    m_opm.writeReg(uint8_t(0x20 + t.ch), uint8_t((t.pan << 6) | t.fbAlg));

    const uint8_t* p = v + 4;
    for (int op = 0; op < 4; ++op) {
        const int off = op * 8;
        t.opTL[op] = p[1 * 4 + op] & 0x7F;
        m_opm.writeReg(uint8_t(0x40 + off + t.ch), p[0 * 4 + op]);   // DT1 / MUL
        m_opm.writeReg(uint8_t(0x60 + off + t.ch), p[1 * 4 + op]);   // TL
        m_opm.writeReg(uint8_t(0x80 + off + t.ch), p[2 * 4 + op]);   // KS / AR
        m_opm.writeReg(uint8_t(0xA0 + off + t.ch), p[3 * 4 + op]);   // AMS / D1R
        m_opm.writeReg(uint8_t(0xC0 + off + t.ch), p[4 * 4 + op]);   // DT2 / D2R
        m_opm.writeReg(uint8_t(0xE0 + off + t.ch), p[5 * 4 + op]);   // D1L / RR
    }
    t.slotMask = v[2] & 0x0F;
    t.voiceVol = v[3] & 0x7F;
    applyVolume(t);
}

// Volume and velocity are attenuations on this driver - `V` is stored as
// 127 - n and `@v` as 255 - n - and on an OPM an attenuation is added to the
// carriers' total level. The two combine the way the manual describes them:
// "the level actually produced is VL, V, @v and the voice's own VOL together".
void MldFm::applyVolume(Track& t)
{
    if (t.ch < 0 || t.ch >= 8) return;
    const int carriers = kCarriers[t.alg & 7];
    // Every term is an attenuation in the chip's own TL units and they simply
    // add, which is what the MXDRV engine next door does with its single term
    // and what the manual describes for this one: "the level actually produced
    // is VL, V, @v and the voice's VOL together". An earlier version halved the
    // sum, which had no basis and left the FM half far too loud against the
    // MIDI half.
    const int atten = (127 - t.vel) + (127 - t.vol) + t.voiceVol;
    for (int op = 0; op < 4; ++op) {
        if (!(carriers & (1 << op))) continue;
        int tl = t.opTL[op] + atten;
        if (tl > 127) tl = 127;
        if (tl < 0) tl = 0;
        m_opm.writeReg(uint8_t(0x60 + op * 8 + t.ch), uint8_t(tl));
    }
}

void MldFm::keyOn(Track& t, int note)
{
    if (t.ch >= 8) {                                  // an ADPCM track
        const int v = (t.ch - 8) & (kPcmVoices - 1);
        if (!m_pdx.empty()) {
            // The note number IS the sample index, the same rule the MXDRV
            // engine follows, and a PDX header holds 96 of them - past that
            // the offsets being read would be sample data.
            const int k = note & 0x7F;
            if (getenv("MLD_KEYLOG")) {
                bool hit = false;
                if (k < 96 && size_t(k * 8 + 8) <= m_pdx.size()) {
                    const uint8_t* e = m_pdx.data() + k * 8;
                    hit = (e[0] | e[1] | e[2] | e[3]) && (e[4] | e[5] | e[6] | e[7]);
                }
                fprintf(stderr, "PCMON %8.3f s  ch %d  note %d  vel 0x%02X  %s\n",
                        double(m_samplePos) / double(m_sampleRate), t.ch, note, t.velByte,
                        hit ? "sample" : "EMPTY");
            }
            if (k < 96 && size_t(k * 8 + 8) <= m_pdx.size()) {
                const uint8_t* e = m_pdx.data() + k * 8;
                const uint32_t off = (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16)
                                   | (uint32_t(e[2]) << 8) | e[3];
                const uint32_t len = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16)
                                   | (uint32_t(e[6]) << 8) | e[7];
                if (off && len && size_t(off) + len <= m_pdx.size()) {
                    // The RAW volume byte, not the loudness. Msm6258::setVolume
                    // resolves it the way the driver does - bit 7 set means the
                    // low seven bits are the attenuation outright, clear means a
                    // 0-15 index into its own curve - and passing a 0-127
                    // loudness instead indexed that curve with nonsense.
                    //
                    // The two drivers agree here, which is the check that says
                    // this is right: MLD's OPM v-curve reads 85, 87, 90 ... 125
                    // as loudness, and 127 minus each of those is 42, 40, 37
                    // ... 2 - exactly MXDRV's attenuation table, entry for
                    // entry.
                    m_pcm[v].setVolume(t.velByte);
                    m_pcm[v].setPan(t.pan);
                    m_pcm[v].start(m_pdx.data() + off, len, false);
                    ++m_keyOns;
                }
            }
        }
        return;
    }
    if (t.ch < 0 || t.ch >= 8) return;

    // The note byte is a MIDI note number - `c4` compiles to 60 on an OPM
    // track exactly as on a MIDI one - and the key code below is the MXDRV
    // engine's formula, whose numbering is NOT MIDI's. The X68000 clocks its
    // OPM at 4 MHz, 1.92 semitones above the 3.58 MHz the chip's key codes are
    // named for, and MXDRV absorbs that in its note numbers plus a key
    // fraction of 5/64: measured on our validated engine, MDX note 45 plays
    // 261.1 Hz, which is MIDI 60. So MIDI note m is MXDRV note m - 15, with
    // the same 0x14 in the key-fraction register. Handing m straight to the
    // formula, as this did, put every OPM note 15 semitones above the MIDI
    // part it was written to double - ken_pcm's lead came out as D#5 over the
    // SC-55's C4, and was heard as "wrong, and out of sync".
    int semi = note - 15 + m_transpose;
    if (semi < 0) semi = 0;
    if (semi > 95) semi = 95;
    const uint8_t kc = uint8_t((semi / 12) * 16 + kNoteField[semi % 12]);
    m_opm.writeReg(uint8_t(0x30 + t.ch), 0x14);                // +5/64 semitone
    m_opm.writeReg(uint8_t(0x28 + t.ch), kc);

    // A tie carries a note that is already sounding: the driver suppresses the
    // key OFF, not the key on, so nothing is re-struck here either.
    if (!t.tie) {
        m_opm.writeReg(0x08, uint8_t(t.ch));                   // key off first
        m_opm.writeReg(0x08, uint8_t((t.slotMask << 3) | t.ch));
        ++m_keyOns;
        // MLD_KEYLOG=1 prints every OPM key-on with its time, so the FM half
        // can be laid beside the MIDI half's note-ons - which is how the two
        // were found to be 0.88 s apart on ken_pcm.
        if (getenv("MLD_KEYLOG"))
            fprintf(stderr, "KEYON %8.3f s  ch %d  note %d\n",
                    double(m_samplePos) / double(m_sampleRate), t.ch, note);
    }
}

void MldFm::keyOff(Track& t)
{
    if (t.ch >= 0 && t.ch < 8) m_opm.writeReg(0x08, uint8_t(t.ch));
}

void MldFm::runTrack(Track& t)
{
    const unsigned char* d = m_data.data();
    const int n = int(m_data.size());
    int guard = 0;
    const int kGuard = (t.end - t.start) * 8 + 512;

    while (t.active && t.wait == 0 && ++guard < kGuard) {
        if (t.p < t.start || t.p >= t.end || t.p >= n) { t.active = false; break; }
        const int opPos = t.p;
        const unsigned char c = d[t.p++];

        if (c < 0x80) {                                        // note + duration
            if (t.p >= t.end) { t.active = false; break; }
            const int dur = d[t.p++];
            int gate;
            if (t.gateQ == 0)        gate = (dur > 1) ? dur - 1 : dur;
            else if (t.gateQ & 0x80) gate = dur - (255 - t.gateQ);
            else                     gate = dur * t.gateQ / 8;
            if (gate < 1) gate = 1;
            if (gate > dur) gate = dur;
            if (t.audible) keyOn(t, c);
            t.tie = false;
            t.gateLeft = gate;
            t.wait = dur;
            t.ticks += dur;
            continue;
        }
        if (c == 0x80) {                                       // rest + duration
            if (t.p >= t.end) { t.active = false; break; }
            t.wait = d[t.p++];
            t.ticks += t.wait;
            t.gateLeft = 0;
            t.tie = false;
            continue;
        }

        switch (c) {
        case 0xE1:                                             // channel
            // A track can change module part-way; follow it.
            if (t.p < t.end) {
                const int a = d[t.p++];
                t.audible = !(a & 0x10);
                t.ch = t.audible ? (a & 0x1F) : -1;
            }
            break;
        case 0xFD:                                             // voice
            if (t.p < t.end) { t.voice = d[t.p++]; if (t.audible) sendVoice(t, t.voice); }
            break;
        case 0xFC:                                             // pan, 0-3 here
            if (t.p < t.end) {
                t.pan = d[t.p++] & 0x03;
                if (t.ch < 8)
                    m_opm.writeReg(uint8_t(0x20 + t.ch), uint8_t((t.pan << 6) | t.fbAlg));
                else
                    m_pcm[(t.ch - 8) & (kPcmVoices - 1)].setPan(t.pan);
            }
            break;
        case 0xE3:                                             // V
            if (t.p < t.end) { t.vol = 127 - (d[t.p++] & 0x7F); applyVolume(t); }
            break;
        case 0xED:                                             // F - ADPCM rate
            // The same command MXDRV has: 0-4 pick the sampling rate, 5 and 6
            // are the two PCM kinds, 7 is off. Both of ken_pcm's drum tracks
            // open with `ED 04`, which used to be read as a note.
            if (t.p < t.end) {
                const int a = d[t.p++];
                if (t.ch >= 8) m_pcm[(t.ch - 8) & (kPcmVoices - 1)].setFormat(a);
            }
            break;
        case 0xFB:                                             // v / @v
            if (t.p < t.end) {
                const int a = d[t.p++];
                // Bit 7 set is the fine form and its low seven bits are the
                // attenuation outright, so the loudness is 255 - n; clear, it
                // indexes the OPM curve above.
                t.velByte = a;
                t.vel = (a & 0x80) ? (255 - a) : kOpmVelCurve[a & 0x0F];
                if (t.vel < 1) t.vel = 1;
                if (t.vel > 127) t.vel = 127;
                applyVolume(t);
            }
            break;
        case 0xFA:                                             // )
            if (t.p < t.end) { t.vel = std::min(127, t.vel + d[t.p++]); applyVolume(t); }
            break;
        case 0xF9:                                             // (
            if (t.p < t.end) { t.vel = std::max(1, t.vel - d[t.p++]); applyVolume(t); }
            break;
        case 0xF8:                                             // q / @q
            if (t.p < t.end) t.gateQ = d[t.p++];
            break;
        case 0xEA:                                             // MH - hardware LFO
            // Five bytes laid out exactly as MXDRV's long 0xEA: waveform (bit 6
            // is the sync flag), frequency, PMD with bit 7 set, AMD, and the
            // channel's PMS/AMS. The LFO itself is one per chip, as it is on
            // the OPM; PMS/AMS is this channel's sensitivity to it.
            if (t.p + 5 <= t.end) {
                if (t.audible && t.ch >= 0 && t.ch < 8) {
                    m_opm.writeReg(0x1B, uint8_t(d[t.p] & 0x03));
                    m_opm.writeReg(0x18, d[t.p + 1]);
                    m_opm.writeReg(0x19, d[t.p + 2]);
                    m_opm.writeReg(0x19, uint8_t(d[t.p + 3] & 0x7F));
                    m_opm.writeReg(uint8_t(0x38 + t.ch), d[t.p + 4]);
                }
                t.pmsAms = d[t.p + 4];
                t.p += 5;
            } else t.p = t.end;
            break;
        case 0xE8:                                             // MHON / MHOF (and MP/MA)
            // Bit 4 switches this channel's hardware-LFO sensitivity on or
            // off (`MHON` = E8 10, `MHOF` = E8 00); the software LFOs the other
            // bits name are not implemented here.
            if (t.p < t.end) {
                const int a = d[t.p++];
                if (t.audible && t.ch >= 0 && t.ch < 8)
                    m_opm.writeReg(uint8_t(0x38 + t.ch), uint8_t((a & 0x10) ? t.pmsAms : 0));
            }
            break;
        case 0xFE:                                             // y - a REGISTER
            // On an FM track `y` is a direct write to the chip, not a control
            // change. That is the one command whose meaning flips completely
            // between the two halves of a song.
            if (t.p + 1 < t.end) { if (t.audible) m_opm.writeReg(d[t.p], d[t.p + 1]); t.p += 2; }
            else t.p = t.end;
            break;
        case 0xFF:                                             // tempo
            if (t.p < t.end) {
                m_timerB = d[t.p++];
                m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
                if (getenv("MLD_KEYLOG"))
                    fprintf(stderr, "TEMPO %8.3f s  timerB %d  (track at 0x%X)\n",
                            double(m_samplePos) / double(m_sampleRate), m_timerB, t.p - 2);
            }
            break;
        case 0xF7:                                             // tie
            t.tie = true;
            break;
        case 0xF6:                                             // loop start
            if (t.p + 1 < t.end) t.loop.push_back(d[t.p]);
            t.p += 2;
            break;
        case 0xF4: {                                           // loop escape
            const int off = signed16At(t.p);
            t.p += 2;
            if (!t.loop.empty() && t.loop.back() <= 1) {
                const int q = opPos + 2 + off;
                if (q >= t.start && q < t.end) t.p = q;
            }
            break;
        }
        case 0xF5: {                                           // loop end
            const int off = signed16At(t.p);
            t.p += 2;
            if (!t.loop.empty()) {
                if (--t.loop.back() >= 1) {
                    const int q = t.p + off;
                    if (q >= t.start && q < t.end) t.p = q; else t.active = false;
                } else {
                    t.loop.pop_back();
                }
            }
            break;
        }
        case 0xF1:
            // The compiler's own "Clock counts" line is the check for this.
            if (getenv("MLD_KEYLOG"))
                fprintf(stderr, "TRACKEND ch %d  %ld clocks\n", t.ch, t.ticks);
            t.active = false;
            break;
        default:
            if (mdxmidi::isBlobCommand(c)) {
                while (t.p < t.end && t.p < n && d[t.p] != 0xF7) ++t.p;
                if (t.p < t.end) ++t.p;
            } else {
                const int k = mdxmidi::operandCount(c);
                if (k > 0) t.p += k;
            }
            break;
        }
    }
}

void MldFm::stepTick()
{
    bool any = false;
    for (size_t k = 0; k < m_tracks.size(); ++k) {
        Track& t = m_tracks[k];
        if (t.gateLeft > 0 && --t.gateLeft == 0 && t.audible) keyOff(t);
        if (t.wait > 0) --t.wait;
        if (t.wait == 0 && t.active) runTrack(t);
        if (t.active) any = true;
    }
    if (!any) m_playing = false;
}

void MldFm::play()
{
    setupTracks();
    m_opm.reset();
    m_tickCounter = 0;
    m_tickAcc = 0.0;
    m_samplePos = 0;
    m_songSamples = 0.0;
    m_playing = true;
    for (size_t k = 0; k < m_tracks.size(); ++k) runTrack(m_tracks[k]);
    // That was tick 0. The next stepTick() is tick 1 and belongs one tick
    // later - with the counter left at zero render16() ran it on the very first
    // sample, and the whole FM half played one tick ahead of its MIDI half
    // (6-14 ms depending on the tempo, measured on every mixed song).
    m_songSamples = m_samplesPerTick;
    m_tickAcc = m_samplesPerTick * 100.0 / double(m_tempoScale);
    m_tickCounter = int(m_tickAcc);
    m_tickAcc -= m_tickCounter;
}

void MldFm::setTempoScale(int percent)
{
    if (percent < 50) percent = 50;
    if (percent > 150) percent = 150;
    m_tempoScale = percent;
}

void MldFm::setTranspose(int semitones)
{
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    m_transpose = semitones;
}

unsigned long MldFm::positionMs() const
{
    return (unsigned long)(m_songSamples * 1000.0 / double(m_sampleRate));
}

// Replays from the top with the chip's writes going straight in and no audio
// rendered, until the song clock reaches `ms`. An FM voice's sound is the sum
// of every register write before it, so there is nothing to jump to - the
// same reasoning as Mxdrv::seekMs. Notes the wind leaves keyed on at the
// target keep sounding, which is what a note held across that point should do.
void MldFm::seekMs(unsigned long ms)
{
    const bool wasPlaying = m_playing;
    play();                                     // from the top, tracks re-armed
    m_opm.setDirectWrites(true);
    const double target = double(ms) * double(m_sampleRate) / 1000.0;
    while (m_playing && m_songSamples < target) {
        stepTick();
        m_songSamples += m_samplesPerTick;
    }
    m_opm.setDirectWrites(false);
    // A sample that was mid-flight at the target is not worth reconstructing.
    for (int v = 0; v < kPcmVoices; ++v) m_pcm[v].stop();
    // m_songSamples is now when the next unplayed tick falls, at or just past
    // the target; wait out the difference rather than firing it at once.
    const double ahead = (m_songSamples - target) * 100.0 / double(m_tempoScale);
    m_tickCounter = ahead > 0.0 ? int(ahead) : 0;
    m_tickAcc = ahead > 0.0 ? ahead - m_tickCounter : 0.0;
    m_samplePos = long(target);
    m_playing = wasPlaying || m_playing;
}

void MldFm::stop()
{
    m_playing = false;
    for (size_t k = 0; k < m_tracks.size(); ++k) if (m_tracks[k].audible) keyOff(m_tracks[k]);
    reset();
}

void MldFm::render16(int16_t* out, int frames)
{
    if (!out || frames <= 0) return;
    std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    if (!m_playing) return;

    int done = 0;
    std::vector<int32_t> L, R, PL, PR;
    while (done < frames) {
        if (m_tickCounter <= 0) {
            stepTick();
            m_songSamples += m_samplesPerTick;
            m_tickAcc += m_samplesPerTick * 100.0 / double(m_tempoScale);
            m_tickCounter = int(m_tickAcc);
            m_tickAcc -= m_tickCounter;
            if (m_tickCounter < 1) m_tickCounter = 1;
            if (!m_playing) break;
        }
        // A player must render in segments that end where its sequencer ticks
        // fall, or every register write in a block lands on its first sample.
        int chunk = frames - done;
        if (chunk > m_tickCounter) chunk = m_tickCounter;
        if (chunk < 1) chunk = 1;

        L.assign(size_t(chunk), 0);
        R.assign(size_t(chunk), 0);
        PL.assign(size_t(chunk), 0);
        PR.assign(size_t(chunk), 0);
        m_opm.render(L.data(), R.data(), chunk);
        // The PCM channels sum on their own and reach the FM through the
        // X68000's output filter, the same stage the MXDRV engine uses - the
        // samples are the same PDX files played by the same PCM8.
        // MDX_NOPCM=1 silences the PCM, for measuring - the same switch the
        // MXDRV engine has.
        static const bool noPcm = getenv("MDX_NOPCM") != nullptr;
        for (int v = 0; v < kPcmVoices && !noPcm; ++v)
            if (m_pcm[v].isPlaying()) m_pcm[v].render(PL.data(), PR.data(), chunk);
        m_pcmBus.mixInto(PL.data(), PR.data(), L.data(), R.data(), chunk);

        // Both sources swing about +-32768 - the OPM's DAC and the ADPCM's
        // 12-bit signal shifted up by four - so the pair needs a divide by
        // 65536 to land inside +-1.0. That constant is measured, and the MXDRV
        // engine next door carries the note explaining how: writing the raw
        // int32 into an int16 clips everything flat, which is exactly what the
        // first version of this did (peak 1.0000 on three of the twelve songs).
        const float gain = m_volume * (1.0f / 65536.0f);
        for (int i = 0; i < chunk; ++i) {
            float l = L[i] * gain, r = R[i] * gain;
            if (l > 1.0f) l = 1.0f; if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; if (r < -1.0f) r = -1.0f;
            out[(done + i) * 2 + 0] = int16_t(l * 32767.0f);
            out[(done + i) * 2 + 1] = int16_t(r * 32767.0f);
        }
        m_tickCounter -= chunk;
        done += chunk;
        m_samplePos += chunk;
    }
}
