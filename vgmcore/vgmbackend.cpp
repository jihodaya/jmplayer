// Video Game Music (.vgm / .vgz).
//
// A VGM is not a score. It is a recording of every write a game made to its
// sound chips, with "wait n samples" markers in between, so playing one back is
// a matter of replaying those writes against an emulation of the same chips at
// the same moments. There is no tempo to get wrong and no sequencer to
// reimplement, which is why this is a very different job from MDX.
//
// What there is to get wrong is the length of every command. The stream is a
// flat byte sequence with no framing, so one command consumed at the wrong
// width turns the rest of the file into noise - exactly the failure MDX had.
// The spec fixes the widths by range, and verify() below checks the whole file
// parses to the sample count its own header declares.
//
// Chip coverage follows what this library actually contains, measured rather
// than guessed. Over the 74 .vgm/.vgz here: SN76489 16, YM2612 16 (the same
// Mega Drive songs), YM3812 16 and YMF262 2 (AdPlug already plays those),
// YM2413 21 with AY8910 alongside, YM2151 13, SegaPCM 12, Game Boy DMG 1,
// YM2610 2, QSound 2.
//
// All of those play. Anything else is parsed for its width and skipped, so an
// unhandled chip costs its own silence and nothing else - which is exactly what
// the YM2413 folder sounded like before this work: the files parsed perfectly
// to their own declared sample counts and played nothing.
#include "vgmbackend.h"

#include "../mdxcore/ym2151.h"
#include "ym2612.h"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace {

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Header offsets, from the VGM specification.
const size_t OFF_EOF        = 0x04;
const size_t OFF_VERSION    = 0x08;
const size_t OFF_SN76489    = 0x0C;
const size_t OFF_TOTAL      = 0x18;
const size_t OFF_LOOP       = 0x1C;
const size_t OFF_YM2413     = 0x10;
const size_t OFF_YM2612     = 0x2C;
const size_t OFF_YM2151     = 0x30;
const size_t OFF_DATA       = 0x34;
const size_t OFF_SEGAPCM    = 0x38;
const size_t OFF_YM2610     = 0x4C;
const size_t OFF_YM3812     = 0x50;
const size_t OFF_YM3526     = 0x54;
const size_t OFF_Y8950      = 0x58;
const size_t OFF_YMF262     = 0x5C;
const size_t OFF_AY8910     = 0x74;
const size_t OFF_AY_TYPE    = 0x78;   // chip type, then three flag bytes
const size_t OFF_QSOUND     = 0xB4;
const size_t OFF_GBDMG      = 0x80;

// A chip's clock only exists if the header is long enough to hold that field.
uint32_t clockAt(const uint8_t* d, size_t size, size_t hdrEnd, size_t off) {
    if (off + 4 > hdrEnd || off + 4 > size) return 0;
    return rd32(d + off) & 0x7FFFFFFF;
}

size_t headerEnd(const uint8_t* d, size_t size) {
    if (size < OFF_DATA + 4) return size;
    const uint32_t rel = rd32(d + OFF_DATA);
    // Before version 1.50 the data always began at 0x40 and this field is zero.
    return rel ? std::min(size, OFF_DATA + size_t(rel)) : size_t(0x40);
}

} // namespace


VgmBackend::VgmBackend() = default;

VgmBackend::~VgmBackend()
{
    delete m_opm;
    delete m_opn2;
}

void VgmBackend::init(int sampleRate)
{
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
}

void VgmBackend::reset()
{
    m_pos = m_dataStart;
    m_samplePos = 0;
    m_waitOut = 0.0;
    m_loopCount = 0;
    m_ended = false;
    m_pcmPos = 0;

    if (m_hasPsg) m_psg.reset();
    if (m_hasOpm && m_opm) m_opm->reset();
    if (m_hasOpn2 && m_opn2) m_opn2->reset();
    if (m_hasDmg) m_dmg.reset();
    if (m_hasOpll) m_opll.reset();
    if (m_hasAy) m_ay.reset();
    if (m_hasSpcm) m_spcm.reset();
    if (m_hasOpnb) m_opnb.reset();
    if (m_hasQsound) m_qsound.reset();
    for (int i = 0; i < 16; ++i) { m_fmLevel[i] = 0; m_fmLastOn[i] = 0; }
    for (int i = 0; i < 11; ++i) { m_opllLevel[i] = 0; m_opllLastOn[i] = 0; }
}


// ---------------------------------------------------------------- gzip

bool VgmBackend::gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B) return false;

    // The trailer's ISIZE is the uncompressed length modulo 4 GB, which is
    // exact for anything of this size and saves growing the buffer blind.
    uint32_t isize = rd32(in.data() + in.size() - 4);
    if (isize == 0 || isize > (64u << 20)) isize = uint32_t(in.size()) * 12 + 4096;
    out.assign(isize, 0);

    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, 15 + 16) != Z_OK) return false;      // +16 = gzip wrapper
    s.next_in = const_cast<Bytef*>(in.data());
    s.avail_in = uInt(in.size());
    s.next_out = out.data();
    s.avail_out = uInt(out.size());
    const int rc = inflate(&s, Z_FINISH);
    const size_t got = out.size() - s.avail_out;
    inflateEnd(&s);
    if (rc != Z_STREAM_END && got == 0) return false;
    out.resize(got);
    return true;
}


// ------------------------------------------------------------- loading

// The whole chip table from the VGM specification, so a file naming something
// exotic can be named rather than guessed at. Order follows the header.
std::string VgmBackend::describeChips(const uint8_t* data, size_t size)
{
    if (!data || size < 0x40 || std::memcmp(data, "Vgm ", 4) != 0) return std::string();
    const size_t he = headerEnd(data, size);

    struct Entry { size_t off; const char* name; bool played; };
    static const Entry kChips[] = {
        { 0x0C, "SN76489",    true  }, { 0x10, "YM2413",   true  },
        { 0x2C, "YM2612",     true  }, { 0x30, "YM2151",   true  },
        { 0x38, "SegaPCM",    true  }, { 0x40, "RF5C68",   false },
        { 0x44, "YM2203",     false }, { 0x48, "YM2608",   false },
        { 0x4C, "YM2610",     false }, { 0x50, "YM3812",   true  },
        { 0x54, "YM3526",     true  }, { 0x58, "Y8950",    true  },
        { 0x5C, "YMF262",     true  }, { 0x60, "YMF278B",  false },
        { 0x64, "YMF271",     false }, { 0x68, "YMZ280B",  false },
        { 0x6C, "RF5C164",    false }, { 0x70, "PWM",      false },
        { 0x74, "AY8910",     true  }, { 0x80, "GB DMG",   true  },
        { 0x84, "NES APU",    false }, { 0x88, "MultiPCM", false },
        { 0x8C, "uPD7759",    false }, { 0x90, "OKIM6258", false },
        { 0x98, "OKIM6295",   false }, { 0x9C, "K051649",  false },
        { 0xA0, "K054539",    false }, { 0xA4, "HuC6280",  false },
        { 0xA8, "C140",       false }, { 0xAC, "K053260",  false },
        { 0xB0, "Pokey",      false }, { 0xB4, "QSound",   true  },
        { 0xB8, "SCSP",       false }, { 0xC0, "WonderSwan", false },
        { 0xC4, "VSU",        false }, { 0xC8, "SAA1099",  false },
        { 0xCC, "ES5503",     false }, { 0xD0, "ES5506",   false },
        { 0xD8, "X1-010",     false }, { 0xDC, "C352",     false },
        { 0xE0, "GA20",       false },
    };

    std::string out;
    for (const Entry& e : kChips) {
        if (!clockAt(data, size, he, e.off)) continue;
        if (!out.empty()) out += ", ";
        out += e.name;
        if (!e.played) out += " (not played)";
    }
    return out;
}

bool VgmBackend::wantsOnlyOpl(const uint8_t* data, size_t size)
{
    if (!data || size < 0x40 || std::memcmp(data, "Vgm ", 4) != 0) return false;
    const size_t he = headerEnd(data, size);

    const bool opl = clockAt(data, size, he, OFF_YMF262) || clockAt(data, size, he, OFF_YM3812)
                  || clockAt(data, size, he, OFF_YM3526) || clockAt(data, size, he, OFF_Y8950);
    if (!opl) return false;

    // If anything else is also declared, AdPlug would play half the song.
    static const size_t others[] = {
        OFF_SN76489, OFF_YM2612, OFF_YM2151, OFF_SEGAPCM, OFF_GBDMG,
        OFF_YM2413, OFF_AY8910, OFF_YM2610, OFF_QSOUND,
    };

    for (size_t off : others) {
        if (clockAt(data, size, he, off)) return false;
    }
    return true;
}

bool VgmBackend::load(const uint8_t* data, size_t size)
{
    if (!data || size < 0x40) return false;

    std::vector<uint8_t> raw;
    if (data[0] == 0x1F && data[1] == 0x8B) {
        std::vector<uint8_t> in(data, data + size);
        if (!gunzip(in, raw)) return false;
    } else {
        raw.assign(data, data + size);
    }
    if (raw.size() < 0x40 || std::memcmp(raw.data(), "Vgm ", 4) != 0) return false;

    m_data.swap(raw);
    const uint8_t* d = m_data.data();
    const size_t n = m_data.size();
    const size_t he = headerEnd(d, n);

    m_version = rd32(d + OFF_VERSION);
    m_declaredSamples = long(rd32(d + OFF_TOTAL));
    m_totalMs = uint32_t((long long)m_declaredSamples * 1000 / 44100);   // VGM counts at 44.1 kHz

    const uint32_t dataRel = (n >= OFF_DATA + 4) ? rd32(d + OFF_DATA) : 0;
    m_dataStart = dataRel ? std::min(n, OFF_DATA + size_t(dataRel)) : size_t(0x40);

    const uint32_t loopRel = rd32(d + OFF_LOOP);
    m_hasLoop = loopRel != 0;
    m_loopStart = m_hasLoop ? std::min(n, OFF_LOOP + size_t(loopRel)) : m_dataStart;

    // Chips
    const uint32_t psgClock  = clockAt(d, n, he, OFF_SN76489);
    m_opn2Clock = int(clockAt(d, n, he, OFF_YM2612));
    m_opmClock  = int(clockAt(d, n, he, OFF_YM2151));

    const uint32_t dmgClock  = clockAt(d, n, he, OFF_GBDMG);
    const uint32_t opllClock = clockAt(d, n, he, OFF_YM2413);
    const uint32_t ayClock   = clockAt(d, n, he, OFF_AY8910);
    const uint32_t spcmClock = clockAt(d, n, he, OFF_SEGAPCM);
    const uint32_t opnbClock = clockAt(d, n, he, OFF_YM2610);
    const uint32_t qsClock   = clockAt(d, n, he, OFF_QSOUND);

    m_hasPsg  = psgClock != 0;
    m_hasOpn2 = m_opn2Clock != 0;
    m_hasOpm  = m_opmClock != 0;
    m_hasDmg  = dmgClock != 0;
    m_hasOpll = opllClock != 0;
    m_hasAy   = ayClock != 0;
    m_hasSpcm = spcmClock != 0;
    m_hasOpnb = opnbClock != 0;
    m_hasQsound = qsClock != 0;

    m_chips.clear();
    if (m_hasPsg)  m_chips += m_chips.empty() ? "SN76489" : " + SN76489";
    if (m_hasOpn2) m_chips += m_chips.empty() ? "YM2612"  : " + YM2612";
    if (m_hasOpm)  m_chips += m_chips.empty() ? "YM2151"  : " + YM2151";
    if (m_hasDmg)  m_chips += m_chips.empty() ? "GB DMG"  : " + GB DMG";
    if (m_hasOpll) m_chips += m_chips.empty() ? "YM2413"  : " + YM2413";
    if (m_hasAy)   m_chips += m_chips.empty() ? "AY8910"  : " + AY8910";
    if (m_hasSpcm) m_chips += m_chips.empty() ? "SegaPCM" : " + SegaPCM";
    if (m_hasOpnb) m_chips += m_chips.empty() ? "YM2610"  : " + YM2610";
    if (m_hasQsound) m_chips += m_chips.empty() ? "QSound" : " + QSound";
    if (m_chips.empty()) m_chips = "(no chip this build plays)";

    if (m_hasPsg) m_psg.init(int(psgClock), m_sampleRate);
    if (m_hasDmg) m_dmg.init(int(dmgClock), m_sampleRate);
    if (m_hasOpll) m_opll.init(int(opllClock), m_sampleRate);
    if (m_hasAy) {
        // The chip type and its flags sit right after the clock, and decide
        // between the AY's coarse volume table and the YM2149's finer one.
        const uint8_t type  = (OFF_AY_TYPE     < he && OFF_AY_TYPE     < n) ? d[OFF_AY_TYPE]     : 0;
        const uint8_t flags = (OFF_AY_TYPE + 1 < he && OFF_AY_TYPE + 1 < n) ? d[OFF_AY_TYPE + 1] : 0;
        m_ay.init(int(ayClock), m_sampleRate, type, flags);
    }
    if (m_hasOpnb) m_opnb.init(int(opnbClock), m_sampleRate);
    if (m_hasQsound) m_qsound.init(int(qsClock), m_sampleRate);
    if (m_hasSpcm) {
        m_spcm.init(int(spcmClock), m_sampleRate);
        // 0x3C says which flag bits pick a ROM bank and how far they shift.
        // Without it every channel reads the first 64 KB of the ROM, and the
        // OutRun files - whose samples start at 0x032600 - played silence.
        m_spcm.setInterface(clockAt(d, n, he, OFF_SEGAPCM + 4));
    }

    if (m_hasOpm) {
        if (!m_opm) m_opm = new Ym2151();
        m_opm->init(m_opmClock, m_sampleRate);
    }

    if (m_hasOpn2) {
        if (!m_opn2) m_opn2 = new Ym2612();
        m_opn2->init(m_opn2Clock, m_sampleRate);
    }

    m_pcmBank.clear();
    m_title.clear();
    m_game.clear();

    // GD3 tag. The strings are UTF-16LE and the order is English first, then
    // Japanese, per field - this had them the other way round AND replaced
    // every non-ASCII unit with '?', so a Neo Geo rip's title bar read
    // "??2???? -MVS Version-" where the file says "Theme of AOF2 -MVS Version-".
    // Kept as UTF-8 here, because this class has no Qt in it.
    const uint32_t gd3Rel = rd32(d + 0x14);
    if (gd3Rel) {
        const size_t g = 0x14 + size_t(gd3Rel);
        if (g + 12 < n && std::memcmp(d + g, "Gd3 ", 4) == 0) {
            size_t p = g + 12;
            auto readString = [&]() {
                std::string s;
                while (p + 1 < n) {
                    uint32_t c = uint16_t(d[p] | (d[p + 1] << 8));
                    p += 2;
                    if (!c) break;
                    // A character outside the basic plane arrives as a
                    // surrogate pair; anything else is its own code point.
                    if (c >= 0xD800 && c <= 0xDBFF && p + 1 < n) {
                        const uint16_t lo = uint16_t(d[p] | (d[p + 1] << 8));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                            p += 2;
                        }
                    }
                    if (c < 0x80) {
                        s += char(c);
                    } else if (c < 0x800) {
                        s += char(0xC0 | (c >> 6));
                        s += char(0x80 | (c & 0x3F));
                    } else if (c < 0x10000) {
                        s += char(0xE0 | (c >> 12));
                        s += char(0x80 | ((c >> 6) & 0x3F));
                        s += char(0x80 | (c & 0x3F));
                    } else {
                        s += char(0xF0 | (c >> 18));
                        s += char(0x80 | ((c >> 12) & 0x3F));
                        s += char(0x80 | ((c >> 6) & 0x3F));
                        s += char(0x80 | (c & 0x3F));
                    }
                }
                return s;
            };
            const std::string trackEn = readString();
            const std::string trackJp = readString();
            const std::string gameEn  = readString();
            const std::string gameJp  = readString();
            // Plenty of rips fill only one language; take whichever is there.
            m_title = trackEn.empty() ? trackJp : trackEn;
            m_game  = gameEn.empty()  ? gameJp  : gameEn;
        }
    }

    reset();
    m_playing = true;
    return true;
}


// ------------------------------------------------------- command widths

// How many bytes follow the command byte. Taken from the VGM specification's
// reserved ranges, which is what makes an unhandled chip harmless: its writes
// are stepped over at the right width instead of being read as commands.
int VgmBackend::commandLength(uint8_t cmd, size_t pos) const
{
    if (cmd >= 0x70 && cmd <= 0x8F) return 0;        // short wait / DAC-and-wait
    if (cmd >= 0x30 && cmd <= 0x3F) return 1;
    if (cmd >= 0x40 && cmd <= 0x4E) return (m_version >= 0x160) ? 2 : 1;
    if (cmd >= 0x51 && cmd <= 0x5F) return 2;
    if (cmd >= 0xA0 && cmd <= 0xBF) return 2;
    if (cmd >= 0xC0 && cmd <= 0xDF) return 3;
    if (cmd >= 0xE0 && cmd <= 0xE1) return 4;

    switch (cmd) {
        case 0x4F: case 0x50: return 1;
        case 0x61:            return 2;
        case 0x62: case 0x63: return 0;
        case 0x66:            return 0;
        case 0x68:            return 11;      // PCM RAM write
        case 0x90: case 0x91: return 4;       // DAC stream control
        case 0x92:            return 5;
        case 0x93:            return 10;
        case 0x94:            return 1;
        case 0x95:            return 4;
        case 0x67: {
            // Data block: 0x67 0x66 tt ssssssss, then ss bytes.
            if (pos + 6 >= m_data.size()) return -1;
            const uint32_t len = rd32(m_data.data() + pos + 3);
            return int(6 + len);
        }
        default: return -1;                   // not in the spec at all
    }
}

VgmBackend::Check VgmBackend::verify() const
{
    Check c;
    c.declaredSamples = m_declaredSamples;

    size_t p = m_dataStart;
    long samples = 0;
    while (p < m_data.size()) {
        const uint8_t cmd = m_data[p];
        if (cmd == 0x66) { c.reachedEnd = true; break; }

        if (cmd >= 0x70 && cmd <= 0x7F)      samples += (cmd & 0x0F) + 1;
        else if (cmd >= 0x80 && cmd <= 0x8F) samples += (cmd & 0x0F);
        else if (cmd == 0x61 && p + 2 < m_data.size())
            samples += m_data[p + 1] | (m_data[p + 2] << 8);
        else if (cmd == 0x62) samples += 735;
        else if (cmd == 0x63) samples += 882;

        const int len = commandLength(cmd, p);
        if (len < 0) {
            if (!c.unknownCommands) c.firstUnknown = cmd;
            ++c.unknownCommands;
            break;
        }
        p += size_t(1 + len);
    }
    c.countedSamples = samples;
    // A file may declare a little more than it plays; a mismatch of more than a
    // frame means the stream was not walked the way the writer wrote it.
    c.samplesMatch = std::llabs((long long)samples - (long long)m_declaredSamples) <= 735;
    return c;
}


// --------------------------------------------------------- chip writes

void VgmBackend::writeChip(uint8_t cmd, uint8_t a, uint8_t b)
{
    switch (cmd) {
        case 0x4F: if (m_hasPsg) m_psg.writeStereo(a); break;
        case 0x50: if (m_hasPsg) m_psg.write(a); break;
        case 0x52:
            // 0xB4-0xB6 is the channel's stereo and LFO sensitivity.
            if (a >= 0xB4 && a <= 0xB6) {
                const int ch = a - 0xB4;
                m_opn2Pan[ch] = b & 0xC0;
                b = uint8_t(pannedOpn2(ch) | (b & 0x3F));
            }
            if (m_hasOpn2 && m_opn2) { m_opn2->write(0, a); m_opn2->write(1, b); }
            // 0x28 is the key-on register: the channel in the low three bits
            // (bit 2 selecting the second bank) and the struck operators above.
            if (a == 0x28) {
                const int ch = (b & 0x03) + ((b & 0x04) ? 3 : 0);
                m_fmLevel[ch & 15] = (b & 0xF0) ? 110 : 0;
                m_fmLastOn[ch & 15] = m_samplePos;
            }
            break;
        case 0x53:
            if (a >= 0xB4 && a <= 0xB6) {
                const int ch = 3 + (a - 0xB4);
                m_opn2Pan[ch] = b & 0xC0;
                b = uint8_t(pannedOpn2(ch) | (b & 0x3F));
            }
            if (m_hasOpn2 && m_opn2) { m_opn2->write(2, a); m_opn2->write(3, b); }
            break;
        case 0x54:
            // 0x20-0x27 carries the channel's stereo bits alongside FB and ALG.
            if (a >= 0x20 && a <= 0x27) {
                const int ch = a & 7;
                m_opmPan[ch] = b & 0xC0;
                m_opmFbAlg[ch] = b & 0x3F;
                b = uint8_t(pannedOpm(ch) | m_opmFbAlg[ch]);
            }
            if (m_hasOpm && m_opm) m_opm->writeReg(a, b);
            // Register 0x08 is the OPM's key-on: low three bits the channel,
            // the four above them which operators are struck.
            if (a == 0x08) {
                const int ch = a ? (b & 0x07) : 0;
                m_fmLevel[ch & 15] = (b & 0x78) ? 110 : 0;
                m_fmLastOn[ch & 15] = m_samplePos;
            }
            break;
        case 0x51:
            if (m_hasOpll) {
                m_opll.writeReg(a, b);
                // 0x20-0x28 is a channel's key-on (bit 4) with the block and
                // the top f-number bit; the monitor follows those, as the FM
                // chips carry no readable level.
                if (a >= 0x20 && a <= 0x28) {
                    const int ch = a - 0x20;
                    m_opllLevel[ch] = (b & 0x10) ? 110 : 0;
                    m_opllLastOn[ch] = m_samplePos;
                }
                // 0x0E strikes the five rhythm voices, which live above the
                // nine melody ones in the monitor.
                if (a == 0x0E && (b & 0x20)) {
                    for (int i = 0; i < 5; ++i) {
                        if (b & (1 << i)) { m_opllLevel[6 + i] = 110; m_opllLastOn[6 + i] = m_samplePos; }
                    }
                }
            }
            break;
        case 0x58: if (m_hasOpnb) m_opnb.write(0, a, b); break;
        case 0x59: if (m_hasOpnb) m_opnb.write(1, a, b); break;
        case 0xA0: if (m_hasAy) m_ay.writeReg(a & 0x0F, b); break;
        case 0xB3: if (m_hasDmg) m_dmg.write(a, b); break;
        default: break;
    }
}

void VgmBackend::handleDataBlock()
{
    // m_pos points at the 0x67.
    if (m_pos + 6 >= m_data.size()) { m_ended = true; return; }
    const uint8_t type = m_data[m_pos + 2];
    const uint32_t len = rd32(m_data.data() + m_pos + 3);
    const size_t body = m_pos + 7;
    if (body + len > m_data.size()) { m_ended = true; return; }

    // Type 0x00 is the YM2612's DAC sample data, which the 0x80-0x8F commands
    // read from. Other block types are ROM images for chips this build does not
    // play, so they are kept out of the way rather than stored.
    if (type == 0x00) {
        m_pcmBank.insert(m_pcmBank.end(), m_data.begin() + body, m_data.begin() + body + len);
    } else if (type == 0x80 && m_hasSpcm && len >= 8) {
        // A ROM-image block: the whole ROM's size, then where this piece of it
        // starts, then the piece. SegaPCM's samples arrive this way, and
        // without them its channels play whatever silence 0x80 encodes.
        const uint32_t romSize = rd32(m_data.data() + body);
        const uint32_t start   = rd32(m_data.data() + body + 4);
        m_spcm.setRom(m_data.data() + body + 8, size_t(len - 8), size_t(start), size_t(romSize));
    } else if ((type == 0x82 || type == 0x83) && m_hasOpnb && len >= 8) {
        // Same shape as SegaPCM's: whole-ROM size, this piece's start, then the
        // piece. 0x82 is ADPCM-A (percussion), 0x83 the delta-T channel.
        const uint32_t romSize = rd32(m_data.data() + body);
        const uint32_t start   = rd32(m_data.data() + body + 4);
        if (type == 0x82)
            m_opnb.setAdpcmRomA(m_data.data() + body + 8, size_t(len - 8), size_t(start), size_t(romSize));
        else
            m_opnb.setAdpcmRomB(m_data.data() + body + 8, size_t(len - 8), size_t(start), size_t(romSize));
    } else if (type == 0x8F && m_hasQsound && len >= 8) {
        const uint32_t romSize = rd32(m_data.data() + body);
        const uint32_t start   = rd32(m_data.data() + body + 4);
        m_qsound.setRom(m_data.data() + body + 8, size_t(len - 8), size_t(start), size_t(romSize));
    }
    m_pos = body + len;
}


// ------------------------------------------------------------ playback

// VGM counts its waits at 44.1 kHz whatever the device is doing.
void VgmBackend::addWait(int vgmSamples)
{
    if (vgmSamples <= 0) return;
    m_waitOut += double(vgmSamples) * double(m_sampleRate) / 44100.0;
}

// Seeking has to apply every register write to arrive in the right state, but
// it does not have to make a sound doing it. The only chip here that cannot
// take a write immediately is the YM2612 - it goes through a queue that the
// chip drains as it runs - so that is the only one that needs clocking.
void VgmBackend::drainChipWrites()
{
    if (m_hasOpn2 && m_opn2) m_opn2->drain();
}

void VgmBackend::runCommands()
{
    while (m_waitOut < 1.0 && !m_ended) {
        if (m_pos >= m_data.size()) { m_ended = true; return; }
        const uint8_t cmd = m_data[m_pos];

        // 0x80-0x8F: send the next DAC byte, then wait the low nibble.
        if (cmd >= 0x80 && cmd <= 0x8F) {
            if (m_hasOpn2 && m_opn2 && m_pcmPos < m_pcmBank.size()) {
                m_opn2->write(0, 0x2A);
                m_opn2->write(1, m_pcmBank[m_pcmPos++]);
            }
            addWait(cmd & 0x0F);
            ++m_pos;
            continue;
        }
        if (cmd >= 0x70 && cmd <= 0x7F) {
            addWait((cmd & 0x0F) + 1);
            ++m_pos;
            continue;
        }

        switch (cmd) {
            case 0x66:
                if (m_hasLoop && (m_maxLoops <= 0 || ++m_loopCount < m_maxLoops)) {
                    m_pos = m_loopStart;
                } else {
                    m_ended = true;
                }
                return;
            case 0x61:
                if (m_pos + 2 < m_data.size())
                    addWait(m_data[m_pos + 1] | (m_data[m_pos + 2] << 8));
                m_pos += 3;
                continue;
            case 0x62: addWait(735); ++m_pos; continue;
            case 0x63: addWait(882); ++m_pos; continue;
            case 0x67: handleDataBlock(); continue;
            case 0xC0:
                // SegaPCM: 0xC0 <offset lo> <offset hi> <data>. Three operand
                // bytes, so it cannot go through writeChip's two.
                if (m_hasSpcm && m_pos + 3 < m_data.size()) {
                    const uint16_t off = uint16_t(m_data[m_pos + 1] | (m_data[m_pos + 2] << 8));
                    m_spcm.write(off, m_data[m_pos + 3]);
                }
                m_pos += 4;
                continue;
            case 0xC4:
                // QSound: 0xC4 <data hi> <data lo> <register>. Big-endian, and
                // three operand bytes, so it takes the same route as 0xC0.
                if (m_hasQsound && m_pos + 3 < m_data.size()) {
                    const uint16_t val = uint16_t((m_data[m_pos + 1] << 8) | m_data[m_pos + 2]);
                    m_qsound.write(m_data[m_pos + 3], val);
                }
                m_pos += 4;
                continue;
            case 0xE0:
                if (m_pos + 4 < m_data.size()) m_pcmPos = rd32(m_data.data() + m_pos + 1);
                m_pos += 5;
                continue;
            default: break;
        }

        const int len = commandLength(cmd, m_pos);
        if (len < 0) { m_ended = true; return; }    // not a command; stop rather than guess
        const uint8_t a = (len >= 1 && m_pos + 1 < m_data.size()) ? m_data[m_pos + 1] : 0;
        const uint8_t b = (len >= 2 && m_pos + 2 < m_data.size()) ? m_data[m_pos + 2] : 0;
        writeChip(cmd, a, b);
        m_pos += size_t(1 + len);
    }
}

void VgmBackend::renderChips(int32_t* l, int32_t* r, int frames)
{
    if (m_hasPsg) m_psg.render(l, r, frames);
    if (m_hasOpm && m_opm) m_opm->render(l, r, frames);

    if (m_hasOpn2 && m_opn2) m_opn2->render(l, r, frames);
    if (m_hasDmg) m_dmg.render(l, r, frames);
    if (m_hasOpll) m_opll.render(l, r, frames);
    if (m_hasAy) m_ay.render(l, r, frames);
    if (m_hasSpcm) m_spcm.render(l, r, frames);
    if (m_hasOpnb) m_opnb.render(l, r, frames);
    if (m_hasQsound) m_qsound.render(l, r, frames);
}

void VgmBackend::render(float* buffer, int frameCount)
{
    if (!m_playing || m_paused || !buffer || frameCount <= 0) return;
    if (m_ended) { m_playing = false; return; }

    std::vector<int32_t> left(size_t(frameCount), 0);
    std::vector<int32_t> right(size_t(frameCount), 0);

    int done = 0;
    while (done < frameCount) {
        if (m_waitOut < 1.0) {
            runCommands();
            if (m_ended && m_waitOut < 1.0) break;
        }
        const int want = std::min(frameCount - done, std::max(1, int(m_waitOut)));
        renderChips(left.data() + done, right.data() + done, want);
        m_waitOut -= double(want);
        if (m_waitOut < 0.0) m_waitOut = 0.0;
        done += want;
        m_samplePos += want;
    }

    const float scale = m_volume * (1.0f / 32768.0f);
    for (int i = 0; i < done; ++i) {
        buffer[i * 2]     += std::clamp(left[i] * scale, -1.0f, 1.0f);
        buffer[i * 2 + 1] += std::clamp(right[i] * scale, -1.0f, 1.0f);
    }
}


// ------------------------------------------------------ channel monitor

int VgmBackend::voiceCount() const
{
    int n = 0;
    if (m_hasOpn2) n += 6;
    if (m_hasOpm)  n += 8;
    if (m_hasPsg)  n += 4;
    if (m_hasDmg)  n += 4;
    if (m_hasOpll) n += m_opll.voiceCount();
    if (m_hasAy)   n += 3;
    if (m_hasSpcm) n += 16;
    if (m_hasOpnb) n += Ym2610::kVoices;
    if (m_hasQsound) n += Qsound::kVoices;
    return n;
}

std::string VgmBackend::voiceName(int i) const
{
    // Was `'1' + k`, which runs off the end of the digits at ten: SegaPCM's
    // sixteen channels came out PCM1..PCM9 then PCM: PCM; PCM<, and QSound's
    // Q1..Q9 then Q: Q; Q< Q= Q> Q? Q@. Nothing had more than eight voices
    // when it was written.
    auto num = [](const char* p, int k) {
        return std::string(p) + std::to_string(k + 1);
    };
    int base = 0;
    if (m_hasOpn2) { if (i - base < 6) return num("FM", i - base); base += 6; }
    if (m_hasOpm)  { if (i - base < 8) return num("FM", i - base); base += 8; }
    if (m_hasPsg)  { if (i - base < 4) return (i - base == 3) ? "PSG N" : num("PSG", i - base); base += 4; }
    if (m_hasDmg) {
        static const char* kDmg[4] = {"SQ1", "SQ2", "WAVE", "NOISE"};
        if (i - base < 4) return kDmg[i - base];
        base += 4;
    }
    if (m_hasOpll) {
        const int n = m_opll.voiceCount();
        if (i - base < n) {
            // In rhythm mode the last five voices are the drum kit, which the
            // chip drives from a single register rather than as channels.
            static const char* kRhythm[5] = {"BD", "SD", "TOM", "CYM", "HH"};
            const int v = i - base;
            if (n == 11 && v >= 6) return kRhythm[v - 6];
            return num("FM", v);
        }
        base += n;
    }
    if (m_hasAy)   { if (i - base < 3) return num("AY", i - base); base += 3; }
    if (m_hasSpcm) { if (i - base < 16) return num("PCM", i - base); base += 16; }
    if (m_hasOpnb) {
        if (i - base < Ym2610::kVoices) {
            const int v = i - base;
            if (v < 4)  return num("FM", v);
            if (v < 7)  return num("SSG", v - 4);
            if (v < 13) return num("ADPCM", v - 7);
            return std::string("DELTA-T");
        }
        base += Ym2610::kVoices;
    }
    if (m_hasQsound) { if (i - base < Qsound::kVoices) return num("Q", i - base); }
    return std::string();
}

int VgmBackend::voiceLevel(int i) const
{
    int base = 0;
    if (m_hasOpn2) {
        if (i - base < 6) {
            // Decays over about a third of a second after the key goes up, so
            // a bar does not sit lit for the rest of the song.
            const int ch = i - base;
            if (!m_fmLevel[ch]) return 0;
            const long long age = m_samplePos - m_fmLastOn[ch];
            const long long span = (long long)m_sampleRate / 3;
            if (age >= span) return 20;
            return int(m_fmLevel[ch] - (m_fmLevel[ch] - 20) * age / span);
        }
        base += 6;
    }
    if (m_hasOpm) {
        if (i - base < 8) {
            const int ch = i - base;
            if (!m_fmLevel[ch]) return 0;
            const long long age = m_samplePos - m_fmLastOn[ch];
            const long long span = (long long)m_sampleRate / 3;
            if (age >= span) return 20;
            return int(m_fmLevel[ch] - (m_fmLevel[ch] - 20) * age / span);
        }
        base += 8;
    }
    if (m_hasPsg) { if (i - base < 4) return m_psg.level(i - base); base += 4; }
    if (m_hasDmg) { if (i - base < 4) return m_dmg.level(i - base); base += 4; }
    if (m_hasOpll) {
        const int n = m_opll.voiceCount();
        if (i - base < n) {
            const int ch = i - base;
            if (!m_opllLevel[ch]) return 0;
            const long long age = m_samplePos - m_opllLastOn[ch];
            const long long span = (long long)m_sampleRate / 3;
            if (age >= span) return 20;
            return int(m_opllLevel[ch] - (m_opllLevel[ch] - 20) * age / span);
        }
        base += n;
    }
    if (m_hasAy)   { if (i - base < 3) return m_ay.level(i - base); base += 3; }
    if (m_hasSpcm) { if (i - base < 16) return m_spcm.level(i - base); base += 16; }
    if (m_hasOpnb) { if (i - base < Ym2610::kVoices) return m_opnb.level(i - base); base += Ym2610::kVoices; }
    if (m_hasQsound) { if (i - base < Qsound::kVoices) return m_qsound.level(i - base); }
    return 0;
}


// ------------------------------------------------- virtual stereo (F12)

// jmp invents stereo for the OPL because an OPL2 has none. Every chip here
// does have it, and the files use it - so the pattern is applied per channel
// and only where the file left that channel in the middle, exactly as the MDX
// engine does it. A deliberate placement is never overwritten.
char VgmBackend::patternFor(int voice) const
{
    if (m_stereoMode <= 1 || m_stereoMode > 9) return 'M';
    static const char* kMaps[10] = {
        "", "MMMMMMMMMMM", "MMRRLLMMMMR", "LLLRRRMMMMR", "LRLRLRMMMMR",
        "RLRLRLMMMMR", "LLLLRRRRMMR", "RRRRLLLLMMR", "RRRLLLRRRLR",
        "LLRRLLRRLLR",
    };
    return kMaps[m_stereoMode][voice % 11];
}

// YM2151: bit 6 is left, bit 7 is right.
uint8_t VgmBackend::pannedOpm(int ch) const
{
    if (m_opmPan[ch] != 0xC0) return m_opmPan[ch];      // the file placed it
    const char p = patternFor(opmVoiceBase() + ch);
    if (p == 'L') return 0x40;
    if (p == 'R') return 0x80;
    return 0xC0;
}

// YM2612: bit 7 is left, bit 6 is right - the other way round from the OPM.
uint8_t VgmBackend::pannedOpn2(int ch) const
{
    if (m_opn2Pan[ch] != 0xC0) return m_opn2Pan[ch];
    const char p = patternFor(ch);
    if (p == 'L') return 0x80;
    if (p == 'R') return 0x40;
    return 0xC0;
}

int VgmBackend::opmVoiceBase() const { return m_hasOpn2 ? 6 : 0; }

void VgmBackend::setStereoMode(int mode)
{
    m_stereoMode = (mode < 1 || mode > 9) ? 1 : mode;
    applyPanOverrides();
}

// Re-place everything that is still sitting where the file left it.
void VgmBackend::applyPanOverrides()
{
    if (m_hasOpn2 && m_opn2) {
        for (int ch = 0; ch < 6; ++ch) {
            const int port = (ch < 3) ? 0 : 2;
            m_opn2->write(port, uint8_t(0xB4 + (ch % 3)));
            m_opn2->write(port + 1, pannedOpn2(ch));
        }
    }
    if (m_hasOpm && m_opm) {
        for (int ch = 0; ch < 8; ++ch)
            m_opm->writeReg(uint8_t(0x20 + ch), uint8_t(pannedOpm(ch) | m_opmFbAlg[ch]));
    }

    // The PSG and the Game Boy route whole channels with one register each, and
    // 0xFF - everything to both sides - is what they reset to. A file that has
    // written its own value has placed its channels and is left alone.
    int base = 0;
    if (m_hasOpn2) base += 6;
    if (m_hasOpm)  base += 8;

    auto fourChannelRouting = [&](int from) {
        uint8_t v = 0;
        for (int ch = 0; ch < 4; ++ch) {
            const char p = patternFor(from + ch);
            if (p != 'R') v |= uint8_t(0x10 << ch);   // high nibble left
            if (p != 'L') v |= uint8_t(0x01 << ch);   // low nibble right
        }
        return v ? v : uint8_t(0xFF);
    };

    if (m_hasPsg && !m_psg.fileSetStereo()) {
        m_psg.setStereo(fourChannelRouting(base));
    }
    if (m_hasPsg) base += 4;
    if (m_hasDmg && m_dmg.routing() == 0xFF) {
        m_dmg.setRouting(fourChannelRouting(base));
    }
}

uint32_t VgmBackend::getElapsedMs() const
{
    if (m_sampleRate <= 0) return 0;
    return uint32_t((long long)m_samplePos * 1000 / m_sampleRate);
}

void VgmBackend::seekMs(uint32_t ms)
{
    // Same reasoning as MDX: a chip's sound is the sum of every write before it,
    // so the only way to arrive somewhere is to replay the writes to get there.
    const bool wasPaused = m_paused;
    reset();
    m_playing = true;
    m_paused = true;

    // Run the commands, apply every write, and skip the sound. Generating the
    // audio and throwing it away was tried first and is what made the window
    // stop responding: this runs on the GUI thread holding the render lock, and
    // a minute of Mega Drive music is three million chip samples.
    const long long target = (long long)m_sampleRate * ms / 1000;
    long long done = 0;
    while (done < target && !m_ended) {
        if (m_waitOut < 1.0) {
            runCommands();
            if (m_ended && m_waitOut < 1.0) break;
        }
        drainChipWrites();
        const long long want = std::max<long long>(1, (long long)m_waitOut);
        m_waitOut -= double(want);
        if (m_waitOut < 0.0) m_waitOut = 0.0;
        done += want;
    }
    m_samplePos = done;
    m_paused = wasPaused;
}
