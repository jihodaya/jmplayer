#include "mxdrv.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace {

uint16_t readBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t readBE32(const uint8_t* p)
{
    return static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

uint8_t trkIdxToKeyOff(int ch)
{
    return static_cast<uint8_t>(ch & 0x07);
}

// MXDRV Note Mapping (0: D#=2, 1: E=4, 2: F=5, 3: F#=6, 4: G=8, 5: G#=9, 6: A=10, 7: A#=12, 8: B=13, 9: C=14, 10: C#=0, 11: D=1)
// How many bytes follow each command.
//
// The driver dispatches these through a table indexed by 0xFF minus the
// command byte, and each handler pulls its own operands off the stream - so the
// counts below are what those handlers read, not a guess. Where a handler
// branches on the channel being PCM the FM count is used here; the PCM tracks
// are handled separately.
//
// This corrects nearly every command the port had: 0xFF is the tempo (@t), not
// the end of a track; the end is 0xF1, and "F1 00" is what closes 749 of the
// corpus's 1574 tracks. Voice change is 0xFD, pan 0xFC, volume 0xFB - the last
// of which is the most used command in the whole corpus at 31,880 occurrences,
// and was being read as pan.

// Which operators are carriers, per algorithm. On an OPM the volume is an
// attenuation added to the carriers' total level, and which operators those are
// depends on the algorithm.
// The driver's own volume curve, for the "v0-v15" form of the volume command.
static const uint8_t kVolumeCurve[16] = {
    0x2A, 0x28, 0x25, 0x22, 0x20, 0x1D, 0x1A, 0x18,
    0x15, 0x12, 0x10, 0x0D, 0x0A, 0x08, 0x05, 0x02,
};

static const uint8_t kCarriers[8] = {
    0x08, 0x08, 0x08, 0x08, 0x0C, 0x0E, 0x0E, 0x0F,
};

static const uint8_t kCmdOperands[0x20] = {
    /* E0 */ 0, /* E1 */ 0, /* E2 */ 0, /* E3 */ 0,
    /* E4 */ 0, /* E5 */ 0, /* E6 */ 0, /* E7 */ 1,
    /* E8 */ 0, /* E9 */ 1, /* EA */ 5, /* EB */ 5,
    /* EC */ 5, /* ED */ 1, /* EE */ 0, /* EF */ 1,
    /* F0 */ 1, /* F1 */ 1, /* F2 */ 2, /* F3 */ 2,
    /* F4 */ 2, /* F5 */ 2, /* F6 */ 2, /* F7 */ 0,
    /* F8 */ 1, /* F9 */ 0, /* FA */ 0, /* FB */ 1,
    /* FC */ 1, /* FD */ 1, /* FE */ 2, /* FF */ 1,
};

static const uint8_t kcTable[12] = {2, 4, 5, 6, 8, 9, 10, 12, 13, 14, 0, 1};

} // namespace

// How long one sequencer tick lasts, in output samples.
//
// This used to be written inline as
//     (m_sampleRate * (256 - m_timerB) * 1024) / 4000000
// which overflows a 32-bit int before it ever divides: at 44.1 kHz with the
// default timer value that product is 2,529,792,000, over INT_MAX. It came out
// negative, the "<= 0" guard below replaced it with a flat 1000, and every
// song therefore played at one fixed speed regardless of its own tempo.
//
// Plain 64-bit arithmetic is all it needs. Measured against mxwav: the onsets
// then sit 0.424 s apart where the reference has 0.430 s.
// Exact, not rounded to a sample: the discarded fraction is a tempo error of
// up to 0.3% at these rates, which is inaudible on an MDX alone and 130 ms a
// pass on an MLD song whose MIDI half keeps wall-clock time (see mldfm.cpp).
static double samplesPerTickFor(int sampleRate, int timerB)
{
    // 1024 - the plain timer-B figure.
    //
    // This was briefly 2048, on the strength of one song where five of our
    // key-ons landed within 0.01 s of the reference's onsets. That was a trap:
    // in that song both values put a note on every other reference onset, so
    // either looked convincing. Scored across all 101 songs on the fraction of
    // reference onsets we place a key-on near, the answer is not close -
    // 1024 gives 0.683, 1536 gives 0.480 and 2048 gives 0.381.
    int mul = 1024;
    { const char* e = getenv("MDX_TICKDIV"); if (e) mul = atoi(e); }
    const double period = double(sampleRate) * double(256 - timerB) * double(mul);
    const double n = period / 4000000.0;
    return (n < 1.0) ? 1.0 : n;
}

Mxdrv::Mxdrv()
    : m_sampleRate(44100)
    , m_numTracks(9)
    , m_voiceTable(nullptr)
    , m_voiceTableSize(0)
    , m_playing(false)
    , m_paused(false)
    , m_volume(1.0f)
    , m_tempoScale(100)
    , m_keyTranspose(0)
    , m_currentLoop(0)
    , m_maxLoops(2)
    , m_elapsedMs(0)
    , m_totalMs(0)
    , m_timerB(200)
    , m_tickCounter(0)
    , m_samplesPerTick(1000.0)
{
    init(m_sampleRate);
}

Mxdrv::~Mxdrv()
{
}

void Mxdrv::init(int sampleRate)
{
    // 49716 was the OPL3's output rate, copied in by mistake; the YM2151 has
    // nothing to do with it. The chip runs at its own rate inside Ym2151 and is
    // resampled, so this is simply whatever the audio device asks for.
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    m_opm.init(4000000, m_sampleRate);
    for (int v = 0; v < kPcmVoices; ++v) m_pcm[v].init(m_sampleRate);
    m_pcmBus.init(m_sampleRate);
    reset();
}

void Mxdrv::reset()
{
    m_playing = false;
    m_paused = false;
    m_currentLoop = 0;
    m_elapsedMs = 0;
    m_samplePos = 0;
    m_keyOns = 0;
    // The length measurement and seeking both run the sequencer with the chip
    // silent, which would otherwise leave their key-ons in this log and confuse
    // the offline comparison harness that reads it.
    m_keyOnTimes.clear();
    m_tickCounter = 0;
    m_tickAcc = 0.0;
    // 0xC8 is what the driver itself writes to timer B when it starts up
    // (mxdrv200b/src/mxdrv.cpp, the D2 = 0xc8 / D1 = 0x12 pair). reset() used
    // to leave m_timerB alone, so a song that never sends @t of its own played
    // at whatever tempo the previous song had left behind.
    m_timerB = 200;
    m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
    // On. MDX_LFO=0 turns it off, which is only useful for measuring against
    // the reference render.
    { const char* e = getenv("MDX_LFO"); m_lfoEnabled = !(e && atoi(e) == 0); }

    m_opm.reset();
    for (int v = 0; v < kPcmVoices; ++v) m_pcm[v].reset();
    m_pcmBus.reset();

    std::memset(m_tracks, 0, sizeof(m_tracks));

    // ...and put them straight back. stop() resets, and nothing afterwards
    // rearmed the tracks, so pressing play again on a song that had been
    // stopped found every track inactive and produced silence - the song only
    // came back after loading a different one. loadMdx, the length measurement
    // and seeking all called setupTracks() for themselves; stop() was the one
    // path that did not.
    setupTracks();
}

bool Mxdrv::loadMdx(const uint8_t* mdxData, size_t mdxSize, const uint8_t* pdxData, size_t pdxSize)
{
    if (!mdxData || mdxSize < 32) return false;

    reset();
    m_mdxData.assign(mdxData, mdxData + mdxSize);
    if (pdxData && pdxSize > 0) {
        m_pdxData.assign(pdxData, pdxData + pdxSize);
    } else {
        m_pdxData.clear();
    }

    const uint8_t* raw = m_mdxData.data();
    size_t len = m_mdxData.size();

    // 1. Extract Title
    size_t titleEnd = 0;
    while (titleEnd < len) {
        if (raw[titleEnd] == 0x0D && titleEnd + 2 < len && raw[titleEnd + 1] == 0x0A && raw[titleEnd + 2] == 0x1A) {
            titleEnd += 3;
            break;
        }
        if (raw[titleEnd] == 0x1A) {
            titleEnd++;
            break;
        }
        titleEnd++;
    }

    if (titleEnd > 0 && titleEnd <= len) {
        m_title.assign(reinterpret_cast<const char*>(raw), titleEnd > 3 ? titleEnd - 3 : titleEnd - 1);
    } else {
        m_title.clear();
    }

    // 2. Extract PDX Name
    m_pdxName.clear();
    while (titleEnd < len && raw[titleEnd] != 0x00) {
        m_pdxName += static_cast<char>(raw[titleEnd]);
        titleEnd++;
    }
    if (titleEnd < len && raw[titleEnd] == 0x00) {
        titleEnd++;
    }

    // 3. Header Base is right after PDX name - usually.
    //
    // ff4_01jsc.mdz has eight bytes sitting between the PDX name's terminator
    // and the offset table, and read from the nominal place its table says the
    // first track starts at 0x500 while a later entry points at 0x100 - data
    // that would sit before the table that describes it. The song loaded as
    // silence. Rather than special-case that file, the base is checked and
    // walked forward until the table it describes is coherent: everything the
    // table points at has to lie at or after the end of the table itself.
    const uint8_t* base = raw + titleEnd;
    {
        const uint8_t* fileEnd = raw + len;
        const uint8_t* found = nullptr;
        for (int adj = 0; adj <= 32 && !found; adj += 2) {
            const uint8_t* b = base + adj;
            if (b + 4 > fileEnd) break;

            const int first = readBE16(b + 2);
            if (first < 4 || (first & 1)) continue;
            if (b + first > fileEnd) continue;

            int nt = (first - 2) / 2;
            if (nt < 1) continue;
            if (nt > MAX_TRACKS) nt = MAX_TRACKS;

            bool ok = (readBE16(b) >= first);           // the voices come after
            for (int t = 0; t < nt && ok; ++t) {
                const int o = readBE16(b + 2 + (t * 2));
                if (o != 0 && o < first) ok = false;     // so does every track
            }
            if (ok) found = b;
        }
        if (found) base = found;
    }
    if (base + 20 > raw + len) return false;

    uint16_t voiceOffset = readBE16(base);
    m_voiceTable = base + voiceOffset;
    m_voiceTableEnd = raw + len;

    // The track count is in the file, not a constant.
    //
    // The first track offset points just past the end of the offset table, so
    // the table's size - and with it the number of tracks - falls out of it.
    // This used to be hardcoded to 9. Measured over all 100 samples here, 95
    // have 16 (eight FM plus eight PCM8 channels) and only 5 have 9 (eight FM
    // plus one ADPCM). So on 95 songs it read nine tracks and then played the
    // remaining seven offset words as if they were music.
    const uint16_t firstOff = readBE16(base + 2);
    int nTracks = (firstOff >= 2) ? ((firstOff - 2) / 2) : 9;
    if (nTracks < 1) nTracks = 1;
    if (nTracks > MAX_TRACKS) nTracks = MAX_TRACKS;
    m_numTracks = nTracks;

    m_trackBase = base;
    setupTracks();

    // 4. Parse PDX Data if present.
    //
    // A PDX is a table of (offset, size) pairs followed by the samples, and
    // the table is 96 entries long - unless it is not. PCM8 lets a song
    // address four banks of 96 with `@n` on a PCM track (the driver forms the
    // index as note + n * 96, mxdrv200b L000f28), and such a PDX simply has a
    // longer table: CAM2.PDX carries 288 entries and its song plays drums out
    // of banks 1 and 2. Nothing in the file states the count, so it is read
    // as "however many entries fit before the first sample begins".
    m_pcmSamples.clear();
    if (!m_pdxData.empty() && m_pdxData.size() >= 8) {
        const uint8_t* pdxRaw = m_pdxData.data();
        const size_t pdxLen = m_pdxData.size();
        size_t tableEnd = pdxLen;
        for (int i = 0; i < 384 && size_t(i) * 8 + 8 <= tableEnd; ++i) {
            const uint32_t offset = readBE32(pdxRaw + (i * 8));
            const uint32_t size = readBE32(pdxRaw + (i * 8) + 4);
            if (offset >= 8 && size > 0 && size_t(offset) + size <= pdxLen) {
                m_pcmSamples.push_back({pdxRaw + offset, size, 15625});
                if (offset < tableEnd) tableEnd = offset;
            } else {
                m_pcmSamples.push_back({nullptr, 0, 15625});
            }
        }
    }

    m_playing = true;
    measureTotalMs();
    return true;
}

void Mxdrv::play()
{
    m_playing = true;
    m_paused = false;
}

void Mxdrv::pause()
{
    m_paused = true;
}

void Mxdrv::stop()
{
    m_playing = false;
    m_paused = false;
    reset();
}

void Mxdrv::setVolume(float vol)
{
    m_volume = std::clamp(vol, 0.0f, 1.0f);
}

void Mxdrv::setTempoScale(int scale)
{
    m_tempoScale = std::clamp(scale, 50, 150);
}

// jmp's F12 patterns spread the OPL's nine mono channels across the stereo
// field, which is worth doing because an OPL2 has no stereo at all.
//
// An OPM does. Its register 0x20 carries two bits per channel and MDX drives
// them with the `p` command - measured over the 101 songs here, **45 place at
// least one channel off centre and 56 leave everything in the middle**. So the
// pattern is applied per channel and only where the song said nothing: a
// deliberate placement is never overwritten, and a song written flat still
// opens up.
void Mxdrv::setStereoMode(int mode)
{
    m_stereoMode = (mode < 1 || mode > 9) ? 1 : mode;
    // Re-place every channel that is still sitting where the file left it.
    for (int ch = 0; ch < 8; ++ch) {
        if (!m_tracks[ch].active) continue;
        m_opm.writeReg(0x20 + ch,
                       (uint8_t)(panBitsFor(ch, m_tracks[ch].pan) | (m_tracks[ch].fbAlg & 0x3F)));
    }
}

// The OPM's own bits: 0x40 is left, 0x80 is right, both is 0xC0.
uint8_t Mxdrv::panBitsFor(int ch, int songPan) const
{
    if (m_stereoMode <= 1 || (songPan & 3) != 3) {
        return (uint8_t)((songPan & 3) << 6);
    }
    // The same nine patterns jjomesynth.cpp holds for the OPL.
    static const char* kMaps[10] = {
        "", "MMMMMMMMMMM", "MMRRLLMMMMR", "LLLRRRMMMMR", "LRLRLRMMMMR",
        "RLRLRLMMMMR", "LLLLRRRRMMR", "RRRRLLLLMMR", "RRRLLLRRRLR",
        "LLRRLLRRLLR",
    };
    const char p = kMaps[m_stereoMode][ch % 11];
    if (p == 'L') return 0x40;
    if (p == 'R') return 0x80;
    return 0xC0;
}

void Mxdrv::setKeyTranspose(int key)
{
    m_keyTranspose = std::clamp(key, -6, 6);
}

// Finds a voice by its NUMBER, which is the first byte of the record.
//
// This used to index the table - m_voiceTable + voiceIdx * 27 - which is only
// right when the voices happen to be numbered 0, 1, 2, ... without a gap.
// Measured over all 100 samples here: the numbers are distinct and ascending in
// every file, but only 28 start at 0 and run contiguously. The other 72 played
// the wrong instrument. That number field exists precisely to be read.
const uint8_t* Mxdrv::findVoice(int voiceNum) const
{
    if (!m_voiceTable || !m_voiceTableEnd) return nullptr;
    for (const uint8_t* v = m_voiceTable; v + 27 <= m_voiceTableEnd; v += 27) {
        if (v[0] == (uint8_t)voiceNum) return v;
    }
    return nullptr;
}

void Mxdrv::sendVoice(int ch, int voiceIdx)
{
    if (ch < 0 || ch >= 8) return;
    const uint8_t* v = findVoice(voiceIdx);
    if (!v) return;

    // A 27-byte record is  [0] voice number, [1] (FB << 3) | ALG,
    // [2] slot mask, [3..26] the operator parameters.
    //
    // Those 24 bytes are PARAMETER-major - four operators to a row, six rows -
    // not six bytes per operator. This read them a byte early and transposed,
    // so every operator got a scrambled envelope and multiple, which is the
    // most direct reason the output was metallic noise.
    //
    // Decided by the register rules rather than by reading anything: TL is
    // 0-127 so its byte never sets bit 7, and KS/AR, AMS/D1R and DT2/D2R all
    // leave bit 5 unused. Across 554 voice records, parameter-major satisfies
    // that 554 times and operator-major 49.
    m_opm.writeReg(0x20 + ch, panBitsFor(ch, m_tracks[ch].pan) | (v[1] & 0x3F));
    m_tracks[ch].alg = v[1] & 0x07;
    m_tracks[ch].fbAlg = v[1] & 0x3F;

    const uint8_t* p = v + 3;

    // Which operator of the record goes to which of the chip's four slots.
    //
    // The chip's registers step M1, M2, C1, C2 (offsets 0, 8, 16, 24), but a
    // voice file conventionally lists them in MML order - M1, C1, M2, C2. Under
    // test while it is measured against mxwav: 0 = straight through,
    // 1 = MML order.
    static const uint8_t kSlotOf[2][4] = { {0, 1, 2, 3}, {0, 2, 1, 3} };
    int order = 0;
    { const char* e = getenv("MDX_OPORDER"); if (e) order = atoi(e) ? 1 : 0; }
    const uint8_t* slotOf = kSlotOf[order];

    for (int op = 0; op < 4; ++op) {
        m_tracks[ch].opTL[slotOf[op]] = p[1 * 4 + op] & 0x7F;
    }
    for (int op = 0; op < 4; ++op) {
        const int opOff = slotOf[op] * 8;
        m_opm.writeReg(0x40 + opOff + ch, p[0 * 4 + op]); // DT1 / MUL
        m_opm.writeReg(0x60 + opOff + ch, p[1 * 4 + op]); // TL
        m_opm.writeReg(0x80 + opOff + ch, p[2 * 4 + op]); // KS / AR
        m_opm.writeReg(0xA0 + opOff + ch, p[3 * 4 + op]); // AMS / D1R
        m_opm.writeReg(0xC0 + opOff + ch, p[4 * 4 + op]); // DT2 / D2R
        m_opm.writeReg(0xE0 + opOff + ch, p[5 * 4 + op]); // D1L / RR
    }

    // Slot mask decides which operators the key-on touches.
    m_tracks[ch].slotMask = (v[2] & 0x0F);
    applyVolume(ch);
}


// Writes the carriers' total level for the channel's current volume. Louder
// means less attenuation, and TL runs the other way round from volume, so the
// two are subtracted.
void Mxdrv::applyVolume(int ch)
{
    if (ch < 0 || ch >= 8) return;
    const Track& trk = m_tracks[ch];
    const uint8_t carriers = kCarriers[trk.alg & 7];

    // The volume byte is two things at once. With bit 7 set the low seven bits
    // are the attenuation outright - the MML "@v" form, and what almost every
    // song here uses, which is why the operand values cluster between 140 and
    // 155. With bit 7 clear it is a 0-15 index into the driver's own curve.
    const int atten = (trk.volume & 0x80) ? (trk.volume & 0x7F)
                                          : kVolumeCurve[trk.volume & 0x0F];
    for (int op = 0; op < 4; ++op) {
        if (!(carriers & (1 << op))) continue;
        int tl = (int)trk.opTL[op] + atten;
        if (tl > 127) tl = 127;
        if (tl < 0) tl = 0;
        m_opm.writeReg(0x60 + op * 8 + ch, (uint8_t)tl);
    }
}

long g_cmdHist[32];

void Mxdrv::parseTrack(Track& trk, int trkIdx)
{
    if (!trk.active || !trk.pc) return;

    const uint8_t* end = m_mdxData.data() + m_mdxData.size();
    int loopSafety = 0;

    while (trk.waitTicks <= 0 && trk.pc < end) {
        if (++loopSafety > 100) {
            trk.waitTicks = 1;
            break;
        }

        uint8_t cmd = *trk.pc++;

        // 0xFF is NOT the end of a track - it is @t, the tempo, and it is
        // handled with the other commands below. There used to be a special
        // case here that treated it as "end of track, loop to the start",
        // which caught it before the switch could: a track opening with @t
        // jumped home every tick, spun until the safety counter tripped, and
        // never played a note. Most songs set the tempo in their first track.

        if (cmd >= 0x80 && cmd <= 0xDF) {
            // Note On (0x80..0xDF): cmd - 0x80 = Note Code in D+ based scale
            int noteCode = (cmd - 0x80) + m_keyTranspose;
            if (noteCode < 0) noteCode = 0;
            if (noteCode > 95) noteCode = 95;

            const uint8_t len = (trk.pc < end) ? *trk.pc++ : 0;

            // Length and gate as mxdrv200b/src/mxdrv.cpp works them out, at
            // L0011dc and L001226, and L001216 adds ONE to each before storing
            // them.
            //
            // That increment was left off for a long time because the corpus
            // said it was worse. The corpus was wrong: its rhythm figure asked
            // whether ANY key-on fell within 50 ms of each reference onset, and
            // at 29 key-ons a second almost anything does - so running FAST
            // scored well, and slowing down to the right speed scored badly.
            //
            // What settled it was a made-up MDX with eight four-tick notes
            // separated by four-tick rests, rendered by mxwav. The reference
            // puts its onsets 143.6 ms apart - 10.01 ticks - where this engine
            // put them 114.7 ms apart, 8.00 ticks. Five and five, not four and
            // four. See tests/probe in the notes.
            //
            // q lives in one byte with two meanings. Below 0x80 it is a ratio
            // out of eight; at or above, it is a negative offset added to the
            // length as a byte, and a note whose length cannot absorb it is cut
            // to a single tick. q starts at 8, the driver's own initial value.
            const int q = trk.q ? trk.q : 8;
            int gate;
            if (q < 0x80) {
                gate = (q * len) >> 3;
            } else {
                const int sum = q + len;
                gate = (sum >= 0x100) ? (sum & 0xFF) : 0;
            }
            trk.waitTicks = int(len) + 1;
            trk.gateTicks = gate + 1;
            if (trk.gateTicks < 1) trk.gateTicks = 1;

            if (!trk.isPcm) {
                // Pitch the way the driver does it: a note is 64 units of
                // pitch, offset by five, plus whatever detune is in force.
                // That value indexes the key code and its low bits become the
                // key fraction, so a detune of less than a semitone is real
                // rather than rounded away.
                //
                // This replaces a note base and an octave offset that had been
                // fitted by ear against the reference. Both existed to cancel
                // errors elsewhere - the sample rate, and the command mapping -
                // and with those fixed they only got in the way.
                int pitch = ((cmd & 0x7F) * 64) + 5 + trk.detune;
                if (pitch < 0) pitch = 0;
                if (pitch > 0x17FF) pitch = 0x17FF;
                trk.pitch = pitch;
                // A key-on only disturbs the LFO when a delay is configured.
                // The driver (mxdrv.cpp around L000c9e) loads S0025 from S0024
                // and, ONLY if that is non-zero, clears the pitch offset and
                // starts counting; with no delay it falls straight through and
                // the LFO keeps free-running across the note. Re-arming on
                // every key-on instead restarts the triangle at its peak
                // displacement each time, which is a different sound and a
                // much larger average offset.
                if (trk.lfoDelay != 0) {
                    trk.lfoDelayLeft = trk.lfoDelay;
                    trk.lfoValue = 0;
                }

                const int fine = (pitch * 4) & 0xFF;   // key fraction
                const int semi = (pitch * 4) >> 8;     // 0..95

                // The key code is not a plain semitone count: an OPM octave has
                // sixteen slots and only twelve are used, with the gaps at 3,
                // 7, 11 and 15.
                static const uint8_t kNoteField[12] =
                    { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
                const uint8_t kc = (uint8_t)((semi / 12) * 16 + kNoteField[semi % 12]);

                m_opm.writeReg(0x30 + trkIdx, (uint8_t)fine);
                m_opm.writeReg(0x28 + trkIdx, kc);
                // A tie carries a note that is already sounding; it does NOT
                // mean "never key on". The driver's tie flag (bit 2 of S0016)
                // is tested in the gate countdown, where it stops the note
                // being keyed OFF, and the key-on stage never looks at it.
                // Skipping the key-on outright silenced any track whose first
                // note is tied - three of DK_03's eight, whose repeat blocks
                // open `tie, note, tie, note` and never sounded at all.
                if (trk.tie && trk.keyedOn) {
                    trk.tie = false;      // pitch moved; envelope keeps running
                } else {
                    trk.tie = false;
                    const uint8_t mask = trk.slotMask ? trk.slotMask : 0x0F;
                    m_opm.writeReg(0x08, (mask << 3) | trkIdx); // Key On
                    trk.keyedOn = true;
                }
                ++m_keyOns;
                if (m_keyOnTimes.size() < 100000)
                    m_keyOnTimes.push_back(m_samplePos * 100 + trkIdx);
            } else {
                // PCM track. The sample is the note, in the bank the track's
                // `@` selected - 96 to a bank, and a song that never says `@`
                // is in bank 0.
                int sampleIdx = trk.voice * 96 + noteCode;
                if (sampleIdx >= 0 && sampleIdx < static_cast<int>(m_pcmSamples.size()) && m_pcmSamples[sampleIdx].data) {
                    if (getenv("MDX_PCMLOG"))
                        fprintf(stderr, "PCM %7.3f s  trk %2d  sample %2d  size %6u  vol %3d  len %d\n",
                                double(m_samplePos)/double(m_sampleRate), trkIdx, sampleIdx,
                                unsigned(m_pcmSamples[sampleIdx].size), int(trk.volume), trk.waitTicks);
                    Msm6258& v = m_pcm[(trkIdx - 8) & (kPcmVoices - 1)];
                    v.setVolume(trk.volume);
                    v.setPan(trk.pan);
                    v.start(m_pcmSamples[sampleIdx].data, m_pcmSamples[sampleIdx].size, false);
                    ++m_keyOns;
                    if (m_keyOnTimes.size() < 100000) m_keyOnTimes.push_back(m_samplePos * 100 + trkIdx);
                }
            }
            break;
        }

        if (cmd < 0x80) {
            // Rest (0x00..0x7F). The driver reaches the same L001216 as a
            // note does, with the rest byte standing in for both fields, so a
            // rest lasts byte+1 ticks too - measured, see above.
            trk.waitTicks = int(cmd) + 1;
            break;
        }

        // Extended commands. The operand bytes are consumed from the table
        // above no matter whether we understand the command, so a command we
        // do not act on can no longer throw the rest of the track out of step.
        const uint8_t* operands = trk.pc;
        int nOperands = kCmdOperands[cmd - 0xE0];

        if (getenv("MDX_CMDHIST")) {
            extern long g_cmdHist[32];
            ++g_cmdHist[cmd - 0xE0];
        }

        // 0xE7 is a second jump table of its own (mxdrv200b L001694): one
        // selector byte, then however many that sub-command wants. Read as a
        // flat one-operand command - which is what the table above used to say
        // - every 0xE7 left one to six bytes of its own operands sitting in the
        // stream, and the track played them as music from there on.
        if (cmd == 0xE7 && operands < end) {
            switch (operands[0]) {
                case 1: case 3: case 5: case 6: nOperands = 2; break;
                case 2:                         nOperands = 7; break;
                case 4: {
                    // "do this on another channel": the channel, then a note,
                    // rest or command written out in full.
                    nOperands = 2;
                    if (operands + 2 < end) {
                        const uint8_t nested = operands[2];
                        nOperands += (nested >= 0x80 && nested <= 0xDF) ? 2 : 1;
                    }
                    break;
                }
                default:
                    trk.active = false;     // selector 0 and 7 are not commands
                    return;
            }
        }

        trk.pc = (operands + nOperands <= end) ? (operands + nOperands) : end;

        // 0xEA, 0xEB and 0xEC share one shape: bit 7 of the first operand means
        // the short form, which is that byte alone. The table has to assume the
        // long one, so wind back. 0xEA already did this in its own handler;
        // 0xEB and 0xEC were declared one byte long and swallowed four bytes of
        // music every time a song set up an LFO the long way.
        if ((cmd == 0xEB || cmd == 0xEC) && nOperands >= 1 && operands < end
            && (operands[0] & 0x80)) {
            trk.pc = operands + 1;
        }

        switch (cmd) {
            case 0xFF: {   // @t - tempo, straight into the OPM's timer B
                if (nOperands >= 1) {
                    m_timerB = operands[0];
                    m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
                }
                break;
            }
            case 0xFE: {   // a register write the song makes itself
                if (nOperands >= 2) {
                    // Register 0x12 is timer B, which is the tempo. The driver
                    // treats a raw write to it exactly like @t and updates its
                    // own tick length too (mxdrv200b L0012a6, the "cmp.b #$12"
                    // branch). We only wrote it to the chip, so a song that
                    // changes speed this way kept the tick length it started
                    // with and drifted against its own music.
                    if (operands[0] == 0x12) {
                        m_timerB = operands[1];
                        m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
                    }
                    if (!trk.isPcm) m_opm.writeReg(operands[0], operands[1]);
                }
                break;
            }
            case 0xFD: {   // @ - voice change
                if (nOperands >= 1) {
                    trk.voice = operands[0];
                    if (!trk.isPcm) sendVoice(trkIdx, trk.voice);
                }
                break;
            }
            case 0xFC: {   // p - pan. The driver keeps it in the top two bits.
                if (nOperands >= 1) {
                    trk.pan = operands[0] & 0x03;
                    if (!trk.isPcm) {
                        m_opm.writeReg(0x20 + trkIdx,
                                       (uint8_t)(panBitsFor(trkIdx, trk.pan) | (trk.fbAlg & 0x3F)));
                    } else {
                        m_pcm[(trkIdx - 8) & (kPcmVoices - 1)].setPan(trk.pan);
                    }
                }
                break;
            }
            case 0xFB: {   // v - volume
                if (nOperands >= 1) {
                    trk.volume = operands[0];
                    if (!trk.isPcm) applyVolume(trkIdx);
                    else m_pcm[(trkIdx - 8) & (kPcmVoices - 1)].setVolume(trk.volume);
                }
                break;
            }
            case 0xFA: {   // ) - one step quieter
                // The volume byte runs two ways (see applyVolume): a plain v
                // counts up towards loud, an @v with bit 7 set counts up
                // towards quiet. The driver (mxdrv200b L001328) steps each in
                // its own direction and stops at its own end - 0 and 0xFF
                // here, 15 and 0x80 for `(`. This stepped both forms the same
                // way, so on the @v form - which is what nearly every song
                // uses - `)` got louder and `(` quieter. FACE04 leans on the
                // pair forty times a track for its swells and measured 3.4 dB
                // over the reference for it.
                if (trk.volume & 0x80) { if (trk.volume != 0xFF) ++trk.volume; }
                else                   { if (trk.volume != 0)    --trk.volume; }
                if (!trk.isPcm) applyVolume(trkIdx);
                break;
            }
            case 0xF9: {   // ( - one step louder
                if (trk.volume & 0x80) { if (trk.volume != 0x80) --trk.volume; }
                else                   { if (trk.volume != 0x0F) ++trk.volume; }
                if (!trk.isPcm) applyVolume(trkIdx);
                break;
            }
            case 0xF8: {   // q - gate ratio
                if (nOperands >= 1) trk.q = operands[0];
                break;
            }
            case 0xF3: {   // D - detune, a signed 16-bit value in 1/64 semitones
                if (nOperands >= 2) {
                    trk.detune = (int16_t)((operands[0] << 8) | operands[1]);
                }
                break;
            }
            case 0xF1: {
                // End of track, or the song's own loop - and the two are not
                // the same length. A zero byte ends it; anything else is the
                // HIGH half of a signed sixteen-bit jump, so a second byte
                // follows that the operand table above does not know about.
                //
                // Treating it as always one byte, and looping to the start of
                // the track instead of to the offset, put track 0 of SF1_02C
                // into a loop it could not leave: every tick it jumped home,
                // spun until the safety counter tripped, and never sounded a
                // note. Three of that song's six tracks were silent.
                if (nOperands < 1) break;
                if (operands[0] == 0x00) {
                    trk.active = false;
                    return;
                }
                if (operands + 2 > end) { trk.active = false; return; }
                trk.pc = operands + 2;                  // the extra byte
                const int off = (int16_t)((operands[0] << 8) | operands[1]);
                const uint8_t* dest = trk.pc + off;
                if (dest > m_mdxData.data() && dest < end) {
                    trk.pc = dest;
                    ++m_currentLoop;
                    // Counted so the length measurement can tell when the song
                    // has been round once. Retiring the track here instead was
                    // tried and is wrong: a track whose whole body is a two-bar
                    // figure jumps back every two bars, so it fell silent while
                    // the rest of the song carried on (rhythm 0.888 -> 0.873
                    // over the 101-song corpus). Playback ends on the clock
                    // instead, in render() below.
                    ++trk.loopedCount;
                }
                break;
            }
            case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            case 0xE4: case 0xE5: case 0xE6: {
                // Not commands. The driver's jump table sends all seven to one
                // routine that points the track at a canned "rest 0x7F, end of
                // data" and gives up on it (mxdrv200b L001442), so meeting one
                // means the stream is no longer being read where the driver
                // reads it.
                //
                // The .mdz files that use them are NOT MDX: they only share the
                // header shape, and their events are encoded the other way round
                // (0x00-0x7F is a MIDI note, 0x80 a rest). They are handled by
                // mdxmidi.cpp, which parses them on their own terms.
                if (getenv("MDX_STRAY"))
                    fprintf(stderr, "[track %d] stray %02X at +%ld\n", trkIdx, cmd,
                            (long)(trk.pc - m_mdxData.data()));
                trk.active = false;
                return;
            }
            case 0xE9: {   // how long after a key-on the LFO waits
                if (nOperands >= 1) trk.lfoDelay = operands[0];
                break;
            }
            case 0xED: {   // F on a PCM track, w on an FM one
                // The driver (mxdrv200b L0014dc): on an FM track the operand
                // goes straight to OPM register 0x0F - noise enable and its
                // frequency; on a PCM track it is the sampling rate and data
                // kind the next key-on will use. Both were consumed and
                // ignored, so PCM recorded at 10.4 kHz played half again too
                // fast (FEILONG.MDX's twelfth track) and no song ever got its
                // noise channel.
                if (nOperands < 1) break;
                if (trk.isPcm) m_pcm[(trkIdx - 8) & (kPcmVoices - 1)].setFormat(operands[0]);
                else m_opm.writeReg(0x0F, operands[0]);
                break;
            }
            case 0xEC: {   // @m - the software pitch LFO
                if (nOperands < 1) break;
                const uint8_t op0 = operands[0];
                if (op0 & 0x80) {
                    // Short form: bit 0 re-arms, otherwise it stops and the
                    // pitch offset is cleared.
                    if (op0 & 0x01) {
                        lfoRearm(trk);
                    } else {
                        trk.lfoOn = false;
                        trk.lfoValue = 0;
                        writePitch(trk, trkIdx);
                    }
                    break;
                }
                if (nOperands < 5) break;

                const int sel = op0 & 3;
                trk.lfoWave = uint8_t(sel + 1);

                const int period = (operands[1] << 8) | operands[2];
                trk.lfoPeriod = period;
                trk.lfoCounterInit = (sel == 1) ? period
                                   : (sel == 3) ? 1 : (period >> 1);

                int32_t step = int32_t(int16_t((operands[3] << 8) | operands[4])) << 8;
                if (op0 >= 4) step <<= 8;      // the deep form
                trk.lfoStepInit = step;
                // Only the SQUARE starts already displaced. The driver's two
                // tests here both read `cmp.b #$02,d1` where d1 is sel doubled
                // (L001536 for the counter, L00155e for the value), so both
                // mean sel == 1 - the same wave that gets the full period
                // rather than half of it. Reading that second test as sel == 2
                // put a step of DC on the triangle, which is 1,506 of the
                // library's 1,507 set-ups.
                trk.lfoValueInit = (sel == 1) ? step : 0;

                trk.lfoOn = true;
                if (!trk.lfoRandom) trk.lfoRandom = 0x12345678u;
                lfoRearm(trk);
                break;
            }
            case 0xEA: {
                // The hardware LFO. Two forms, and they are different lengths:
                // with bit 7 of the first operand set it is a single byte that
                // just switches the LFO off or back on for this channel, and
                // otherwise it is five bytes that set the whole thing up. The
                // operand table above assumes the long form, so the short one
                // has to wind the position back.
                if (nOperands < 1) break;
                if (operands[0] & 0x80) {
                    trk.pc = operands + 1;
                    if (!trk.isPcm) {
                        const uint8_t v = (operands[0] & 0x01) ? trk.pmsAms : 0;
                        m_opm.writeReg(0x38 + trkIdx, v);
                    }
                    break;
                }
                if (nOperands >= 5 && !trk.isPcm) {
                    m_opm.writeReg(0x1B, operands[0] & 0x3F);   // waveform
                    m_opm.writeReg(0x18, operands[1]);          // frequency
                    m_opm.writeReg(0x19, operands[2]);          // PMD or AMD
                    m_opm.writeReg(0x19, operands[3]);          // the other one
                    trk.pmsAms = operands[4];
                    m_opm.writeReg(0x38 + trkIdx, operands[4]); // PMS / AMS
                }
                break;
            }
            case 0xF7: {   // tie - the next note carries the last one on
                // The driver sets a bit on the channel and the note that
                // follows reads it instead of keying on again. SF1_02C ties
                // half the notes of two of its tracks.
                trk.tie = true;
                break;
            }
            case 0xF6: {   // loop start
                // The driver arms the loop by copying the count into the byte
                // that follows it - the song modifies itself, and that second
                // byte is the working counter the loop end decrements. Without
                // this the counter starts at whatever the file shipped, which
                // is zero, and the first decrement wraps it to 255.
                if (nOperands >= 2) {
                    uint8_t* w = m_mdxData.data();
                    const size_t at = (size_t)((operands + 1) - w);
                    if (at < m_mdxData.size()) w[at] = operands[0];
                }
                break;
            }
            case 0xF5: {   // loop end
                // The offset is a signed word and is added to the position
                // just past it, so it is normally negative and lands back at
                // the top of the loop. The count lives in the song itself, one
                // byte before that landing point: the driver decrements it in
                // place and falls through when it reaches zero.
                //
                // The sign was inverted here, which turned every loop into a
                // jump forwards. Track 0 of SF2D_4E opens with six rests
                // wrapped in one of these, so it sat silent through the whole
                // introduction instead of counting the bars out.
                if (nOperands >= 2) {
                    const int off = (int16_t)((operands[0] << 8) | operands[1]);
                    const uint8_t* dest = operands + 2 + off;
                    uint8_t* base = m_mdxData.data();
                    uint8_t* counter = base + ((dest - 1) - base);
                    if (dest > base && dest < end &&
                        counter >= base && counter < base + m_mdxData.size()) {
                        if (--(*counter) != 0) {
                            trk.pc = dest;
                        }
                    }
                }
                break;
            }
            case 0xF4: {   // loop escape - on the LAST pass, leave the loop here
                // mxdrv200b L00139a. The operand is an unsigned word, added to
                // the position just past it, and lands on the OPERAND of the
                // loop's closing 0xF5. That F5's own (negative) offset leads
                // back to the loop's counter; if the counter reads 1 this is
                // the final pass, and play resumes just after the F5 - the rest
                // of the body is skipped.
                //
                // Consumed and ignored until 2026-09-28, which is not harmless:
                // the last pass then played the whole body, so every track
                // that uses one ran long by the skipped tail each time round,
                // and drifted against the tracks that do not. Reported against
                // MXV as "each channel's beat is slightly off" on SF2_KEN,
                // whose eight melodic tracks use 44 of them between them.
                // MDX_NOF4=1 restores the old behaviour, for measuring.
                if (nOperands >= 2 && !getenv("MDX_NOF4")) {
                    const int fwd = (operands[0] << 8) | operands[1];
                    const uint8_t* f5op = operands + 2 + fwd;
                    const uint8_t* base = m_mdxData.data();
                    if (f5op >= base && f5op + 2 <= end) {
                        const int back = (int16_t)((f5op[0] << 8) | f5op[1]);
                        const uint8_t* after = f5op + 2;
                        const uint8_t* counter = after + back - 1;
                        if (counter >= base && counter < end && *counter == 1)
                            trk.pc = after;
                    }
                }
                break;
            }
            default:
                // Everything else has had its operands stepped over already.
                break;
        }
    }
}

// Puts every track back at its first byte. loadMdx does this once; seeking and
// the length measurement below both replay from the start, so they need it too.
void Mxdrv::setupTracks()
{
    const uint8_t* raw = m_mdxData.data();
    const size_t len = m_mdxData.size();
    const uint8_t* base = m_trackBase;
    if (!raw || !base) return;

    std::memset(m_tracks, 0, sizeof(m_tracks));
    for (int t = 0; t < m_numTracks; ++t) {
        uint16_t trkOff = readBE16(base + 2 + (t * 2));
        if (trkOff > 0 && base + trkOff < raw + len) {
            m_tracks[t].active = true;
            m_tracks[t].pc = base + trkOff;
            m_tracks[t].startPc = m_tracks[t].pc;
            // Eight FM channels; anything past them is PCM.
            m_tracks[t].isPcm = (t >= 8);
            m_tracks[t].pan = 3;
            // The driver starts every track at volume 8 (mxdrv200b line 2981,
            // `S0022 = 0x08`), which is the middle of its 0-15 curve. This was
            // 127, and 127 with bit 7 clear indexes that curve at 15 - the
            // loudest entry there is. Any track that never sets its own volume
            // played at full blast, which on this library is most of the drum
            // tracks: every hit at maximum, no matter what.
            m_tracks[t].volume = 8;
        } else {
            m_tracks[t].active = false;
        }
    }

    // Diagnostic: play one track alone, to find which carries a band.
    { const char* e = getenv("MDX_ONLY");
      if (e) { const int only = atoi(e);
               for (int t = 0; t < m_numTracks; ++t) if (t != only) m_tracks[t].active = false; } }
}

// How long the song is, in milliseconds.
//
// This was a flat 180000 - three minutes for everything - which is why the
// progress bar was meaningless. There is no length field in an MDX, so the
// only way to know is to play it, and the sequencer is cheap enough to do that
// with the chip switched off: one pass over a long song is a few milliseconds.
//
// A song that loops has no end, so "one pass" is taken to be the point where
// every track has either run out or jumped back at least once, and the figure
// reported is that pass repeated m_maxLoops times.
void Mxdrv::measureTotalMs()
{
    m_opm.setDirectWrites(true);
    setupTracks();
    m_timerB = 200;
    m_samplesPerTick = samplesPerTickFor(m_sampleRate, m_timerB);
    m_currentLoop = 0;
    m_playing = true;

    const double cap = double(m_sampleRate) * 60.0 * 20.0;   // 20 minutes
    double samples = 0.0;
    bool looped = false;

    while (m_playing && samples < cap) {
        stepSequencer();
        samples += m_samplesPerTick;

        // The pass ends when every track has either run out or come round
        // once. The first jump was tried instead and is wrong: a percussion
        // ostinato jumps back every couple of bars, which cut SF2D_4E from
        // 61 s to 5 s. The longest track's own body is the song.
        //
        // A song where every track RUNS OUT has simply ended, and is not a
        // loop. This used to reach the same `allDone` - an inactive track
        // counts as done - and so doubled the length of every song that ends
        // on F1 00 everywhere. G2FENDV is 103.5 s and reported 207; the bar
        // then mapped anything past its middle to a point after the end, the
        // seek wound the sequencer off the end of the song, and the playlist
        // moved on - "seeking past 50 % skips to the next song" (2026-09-28).
        bool allDone = true, anyLooped = false;
        for (int t = 0; t < m_numTracks; ++t) {
            if (m_tracks[t].loopedCount > 0) anyLooped = true;
            if (m_tracks[t].active && m_tracks[t].loopedCount == 0) { allDone = false; break; }
        }
        if (allDone) { looped = anyLooped; break; }
    }

    const double total = looped ? samples * double(m_maxLoops > 0 ? m_maxLoops : 1) : samples;
    m_totalMs = (uint32_t)(total * 1000.0 / double(m_sampleRate));

    // A song whose tracks all die in the first moments has no length worth
    // reporting - ff4_01jsc.mdz is one, and it made the progress bar jump
    // straight to 100 % and stay there. Zero means "unknown", which stops
    // render() ending playback on the clock and leaves the player its default.
    if (m_totalMs < 1000) m_totalMs = 0;

    // Put everything back the way it was found.
    m_opm.setDirectWrites(false);
    reset();
    setupTracks();
    m_playing = true;
}

// The elapsed time comes from the sample counter rather than being accumulated
// per render call. "m_elapsedMs += frameCount * 1000 / m_sampleRate" truncated
// every single call - at 512 frames and 44.1 kHz it added 11 ms instead of
// 11.61 - so the clock ran about five percent slow and the drift never stopped
// growing.

// What the channel monitor draws. A track counts as sounding while its gate is
// open, or while a tie is holding the note over, and how loud it looks comes
// from the same attenuation the chip is being given - so the bars follow the
// song's own volume commands rather than a level meter on the mixed output.
int Mxdrv::voiceLevel(int track) const
{
    if (track < 0 || track >= m_numTracks) return 0;
    const Track& trk = m_tracks[track];
    if (!trk.active) return 0;
    if (!trk.tie && trk.gateTicks <= 0) return 0;

    const int atten = (trk.volume & 0x80) ? (trk.volume & 0x7F)
                                          : kVolumeCurve[trk.volume & 0x0F];
    int level = 127 - atten;
    if (level < 1) level = 1;       // sounding, however quietly
    if (level > 127) level = 127;
    return level;
}

int Mxdrv::voiceNumber(int track) const
{
    if (track < 0 || track >= m_numTracks) return -1;
    return m_tracks[track].active ? m_tracks[track].voice : -1;
}

std::vector<int> Mxdrv::voiceNumbers() const
{
    std::vector<int> out;
    if (!m_voiceTable || !m_voiceTableEnd) return out;
    for (const uint8_t* v = m_voiceTable; v + 27 <= m_voiceTableEnd; v += 27) {
        out.push_back(v[0]);
    }
    return out;
}

int Mxdrv::currentBpm() const
{
    const int period = 256 - (m_timerB & 0xFF);
    if (period <= 0) return 0;
    const double ticksPerSec = 4000000.0 / (1024.0 * double(period));
    return int(ticksPerSec * 60.0 / 48.0 + 0.5);
}

uint32_t Mxdrv::getElapsedMs() const
{
    if (m_sampleRate <= 0) return 0;
    return (uint32_t)((long long)m_samplePos * 1000 / m_sampleRate);
}

// Seeking replays the song from the start with the audio switched off. That is
// slower than jumping, but an FM song's sound depends on every register write
// that came before it, so this is the only way the chip arrives in the right
// state.
void Mxdrv::seekMs(uint32_t ms)
{
    const bool wasPaused = m_paused;
    reset();
    setupTracks();
    m_playing = true;
    m_paused = true;    // keep render() out while we wind forward
    m_opm.setDirectWrites(true);

    const double target = double(m_sampleRate) * double(ms) / 1000.0;
    double samples = 0.0;
    while (m_playing && samples < target) {
        stepSequencer();
        samples += m_samplesPerTick;
    }

    m_opm.setDirectWrites(false);
    m_samplePos = (long)samples;
    m_tickCounter = 0;
    m_tickAcc = 0.0;
    m_paused = wasPaused;
}



// ------------------------------------------------- software pitch LFO (0xEC)

void Mxdrv::lfoRearm(Track& trk)
{
    trk.lfoCounter = trk.lfoCounterInit;
    trk.lfoStep = trk.lfoStepInit;
    trk.lfoValue = trk.lfoValueInit;
}

// One tick. The driver dispatches through a four-entry jump table and the
// shapes differ in more than sign: the sawtooth flips its accumulated VALUE at
// the end of a cycle where the others flip the STEP, and the fourth is random.
// The first half-cycle is shorter than the rest - the counter starts at
// `period / 2` and reloads to `period`.
void Mxdrv::lfoTick(Track& trk, int trkIdx)
{
    // The driver's per-tick routine (L001050) returns immediately for a PCM
    // track, before it ever reaches the LFO.
    if (!m_lfoEnabled || trk.isPcm || !trk.lfoOn || trk.lfoWave == 0) return;

    if (trk.lfoDelayLeft > 0) {
        if (--trk.lfoDelayLeft == 0) lfoRearm(trk);
        return;
    }

    const int32_t before = trk.lfoValue;
    switch (trk.lfoWave) {
        case 1:
            trk.lfoValue += trk.lfoStep;
            if (--trk.lfoCounter == 0) {
                trk.lfoCounter = trk.lfoPeriod;
                trk.lfoValue = -trk.lfoValue;
            }
            break;
        case 2:
            trk.lfoValue = trk.lfoStep;
            if (--trk.lfoCounter == 0) {
                trk.lfoCounter = trk.lfoPeriod;
                trk.lfoStep = -trk.lfoStep;
            }
            break;
        case 4:
            if (--trk.lfoCounter == 0) {
                trk.lfoRandom = trk.lfoRandom * 1103515245u + 12345u;
                const int16_t r = int16_t(trk.lfoRandom >> 16);
                trk.lfoValue = int32_t(r) * (trk.lfoStep >> 16);
                trk.lfoCounter = trk.lfoPeriod;
            }
            break;
        default:
            trk.lfoValue += trk.lfoStep;
            if (--trk.lfoCounter == 0) {
                trk.lfoCounter = trk.lfoPeriod;
                trk.lfoStep = -trk.lfoStep;
            }
            break;
    }
    if (trk.lfoCounter < 0) trk.lfoCounter = trk.lfoPeriod;

    if (getenv("MDX_LFODUMP")) {
        // The mean is the number that matters: a triangle that is not centred
        // on zero detunes the whole song, and a whole semitone is 64 units.
        static int32_t lo = 0, hi = 0;
        static double sum = 0.0;
        static long n = 0;
        const int32_t u = trk.lfoValue >> 16;
        if (u < lo) lo = u;
        if (u > hi) hi = u;
        sum += u;
        if (++n % 20000 == 0)
            fprintf(stderr, "lfo range %d..%d mean %.2f units (%.3f semitone)\n",
                    lo, hi, sum / double(n), sum / double(n) / 64.0);
    }
    if (before != trk.lfoValue) writePitch(trk, trkIdx);
}

// The note's pitch plus wherever the LFO has wandered. The driver rewrites the
// key code every tick from `S0012 + (S0036 >> 16)`, which is what makes the
// vibrato audible - writing it only at the key-on leaves the note dead still.
void Mxdrv::writePitch(Track& trk, int trkIdx)
{
    if (trk.isPcm || !trk.keyedOn) return;

    int pitch = trk.pitch + (trk.lfoOn ? (trk.lfoValue >> 16) : 0);
    if (pitch < 0) pitch = 0;
    if (pitch > 0x17FF) pitch = 0x17FF;

    static const uint8_t kNoteField[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
    const int semi = (pitch * 4) >> 8;
    m_opm.writeReg(0x30 + trkIdx, (uint8_t)((pitch * 4) & 0xFF));
    m_opm.writeReg(0x28 + trkIdx, (uint8_t)((semi / 12) * 16 + kNoteField[semi % 12]));
}

void Mxdrv::stepSequencer()
{

    // Optional trace of what every track is doing, for the offline harness.
    if (getenv("MDX_TRACE")) {
        static long step = 0;
        const long every = atol(getenv("MDX_TRACE"));
        if (every > 0 && (step % every) == 0) {
            fprintf(stderr, "[t%ld]", step);
            for (int t = 0; t < m_numTracks; ++t) {
                if (!m_tracks[t].active) continue;
                fprintf(stderr, " %d:pc+%ld,w%d,g%d", t,
                        (long)(m_tracks[t].pc - m_mdxData.data()),
                        m_tracks[t].waitTicks, m_tracks[t].gateTicks);
            }
            fprintf(stderr, "\n");
        }
        ++step;
    }

    bool anyActive = false;
    for (int t = 0; t < m_numTracks; ++t) {
        if (!m_tracks[t].active) continue;
        anyActive = true;

        if (m_tracks[t].waitTicks > 0) {
            m_tracks[t].waitTicks--;
        }
        if (m_tracks[t].tie) {
            // Held: do not let the gate cut a note the next one takes over.
        } else if (m_tracks[t].gateTicks > 0) {
            m_tracks[t].gateTicks--;
            if (m_tracks[t].gateTicks == 0) {
                if (!m_tracks[t].isPcm) {
                    m_opm.writeReg(0x08, trkIdxToKeyOff(t)); // Key Off
                    m_tracks[t].keyedOn = false;
                } else {
                    // The driver stops a PCM note at its gate too (L001012,
                    // the PCM8 call). Letting every sample run to its end is a
                    // good deal more sound than the song asked for.
                    m_pcm[(t - 8) & (kPcmVoices - 1)].stop();
                }
            }
        }

        lfoTick(m_tracks[t], t);

        if (m_tracks[t].waitTicks <= 0) {
            parseTrack(m_tracks[t], t);
        }
    }

    if (!anyActive) {
        m_playing = false;
    }
}

void Mxdrv::render(float* buffer, int frameCount)
{
    if (!m_playing || m_paused || !buffer || frameCount <= 0) {
        return;
    }

    if (static_cast<int>(m_leftBuf.size()) < frameCount) {
        m_leftBuf.resize(frameCount);
        m_rightBuf.resize(frameCount);
        m_pcmLeft.resize(frameCount);
        m_pcmRight.resize(frameCount);
    }

    std::fill(m_leftBuf.begin(), m_leftBuf.begin() + frameCount, 0);
    std::fill(m_rightBuf.begin(), m_rightBuf.begin() + frameCount, 0);
    std::fill(m_pcmLeft.begin(), m_pcmLeft.begin() + frameCount, 0);
    std::fill(m_pcmRight.begin(), m_pcmRight.begin() + frameCount, 0);

    // A looping song has no natural end, so it stops at the measured length -
    // one pass repeated m_maxLoops times. Without this the playlist never moved
    // on from an MDX.
    if (m_totalMs > 0 && getElapsedMs() >= m_totalMs) {
        m_playing = false;
        return;
    }

    int samplesProcessed = 0;
    while (samplesProcessed < frameCount) {
        if (m_tickCounter <= 0) {
            stepSequencer();
            m_tickAcc += m_samplesPerTick * 100.0 / double(m_tempoScale);
            m_tickCounter = int(m_tickAcc);
            m_tickAcc -= m_tickCounter;
            if (m_tickCounter < 1) m_tickCounter = 1;
        }

        int chunk = std::min(frameCount - samplesProcessed, m_tickCounter);
        m_opm.render(m_leftBuf.data() + samplesProcessed, m_rightBuf.data() + samplesProcessed, chunk);
        if (!getenv("MDX_NOPCM"))
            for (int v = 0; v < kPcmVoices; ++v)
                m_pcm[v].render(m_pcmLeft.data() + samplesProcessed,
                                m_pcmRight.data() + samplesProcessed, chunk);

        m_tickCounter -= chunk;
        samplesProcessed += chunk;
        m_samplePos += chunk;
    }

    // Both sources now swing about +-32768 - the OPM's DAC and the ADPCM's
    // 12-bit signal shifted up by four - so the pair together needs a divide by
    // 65536 to land inside +-1.0 without clipping. The old 1/4096 was inherited
    // from an emulator with a much smaller range and was clipping everything
    // flat: the FM alone measured RMS 0.212 and the mix 0.686, against the
    // reference's 0.068.
    // The PCM sum goes through the X68000's output filter and joins the FM
    // at the measured level - X68PcmBus, msm6258.h.
    m_pcmBus.mixInto(m_pcmLeft.data(), m_pcmRight.data(),
                     m_leftBuf.data(), m_rightBuf.data(), frameCount);

    float masterVol = m_volume * (1.0f / 65536.0f);
    for (int i = 0; i < frameCount; ++i) {
        float l = m_leftBuf[i] * masterVol;
        float r = m_rightBuf[i] * masterVol;
        buffer[i * 2]     += std::clamp(l, -1.0f, 1.0f);
        buffer[i * 2 + 1] += std::clamp(r, -1.0f, 1.0f);
    }

}

void Mxdrv::render16(int16_t* buffer, int frameCount)
{
    if (!buffer || frameCount <= 0) return;
    std::vector<float> fbuf(frameCount * 2, 0.0f);
    render(fbuf.data(), frameCount);

    for (int i = 0; i < frameCount * 2; ++i) {
        float s = std::clamp(fbuf[i], -1.0f, 1.0f);
        buffer[i] = static_cast<int16_t>(s * 32767.0f);
    }
}


