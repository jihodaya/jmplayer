#include "rcpfilehandler.h"
#include <cstdlib>
#include <cstdio>
#include "midiplayer.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include <windows.h>

namespace {

// Recomposer is a Japanese program and its text fields are Shift-JIS, always.
//
// The player's general decoder cannot assume that: it serves a library that is
// overwhelmingly Korean, so it tries CP949 first and only falls back to
// Shift-JIS. BAMBOO.RCP's title is a run of full-width Latin that happens to
// be valid CP949 as well, and lands on Hangul, so it passed that test and came
// out as a string of unrelated Korean syllables. Here the format settles the
// question, so ask for Shift-JIS by name and keep the general decoder as the
// fallback for anything it rejects.
QString decodeRcpText(const QByteArray& raw)
{
    if (raw.isEmpty()) return QString();

    bool ascii = true;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x80) { ascii = false; break; }
    }
    if (ascii) return QString::fromLatin1(raw);

    const int len = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                        raw.constData(), raw.size(), nullptr, 0);
    if (len > 0) {
        std::wstring buf(len, L'\0');
        if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                raw.constData(), raw.size(), buf.data(), len) > 0) {
            return QString::fromWCharArray(buf.data(), len);
        }
    }
    return MidiPlayer::decodeMetaText(raw);
}

void writeVarLen(std::vector<uint8_t>& buf, uint32_t value)
{
    uint32_t buffer = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80;
        buffer |= (value & 0x7F);
    }
    while (true) {
        buf.push_back(static_cast<uint8_t>(buffer & 0xFF));
        if (buffer & 0x80) {
            buffer >>= 8;
        } else {
            break;
        }
    }
}

void writeBigEndian16(std::vector<uint8_t>& buf, uint16_t val)
{
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void writeBigEndian32(std::vector<uint8_t>& buf, uint32_t val)
{
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint16_t readLE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// ---------------------------------------------------------------------------
// The control file, and why it matters more than anything else here.
//
// A Recomposer song does not carry its module setup. It names a separate file -
// `.GSD` for an SC-55, `.CM6` for a CM-64/MT-32 - in its header at 0x1D6 and
// 0x1C6, and the sequencer sent that file to the module as a bulk dump before
// the first note. Everything that decides how the song actually sounds is in
// there: which patch each part plays, its volume and pan, the reverb and chorus
// settings, and - the audible one - **which parts are rhythm parts**.
//
// Measured against the reference .MID that ships beside BAMBOO.RCP and
// 3DAYS.RCP, dropping this costs 211 and 185 SysEx messages respectively, and
// it is why the drums appeared to be on the wrong channel: the reference keeps
// them on MIDI channel 2 because the GSD declares part 2 as a rhythm part.
// Without the dump, channel 2 is a piano, and a "map DRUM tracks to channel 10"
// workaround was papering over it.
//
// Layout and addresses follow shingo45endo/rcm2smf (MIT), which is the
// reference implementation for this format.
// ---------------------------------------------------------------------------

static uint8_t gsChecksum(const std::vector<uint8_t>& body)
{
    int sum = 0;
    for (uint8_t b : body) sum += b;
    return static_cast<uint8_t>((0x80 - (sum & 0x7F)) & 0x7F);
}

// F0 41 10 42 12 <addr> <data...> <sum> F7 - a Roland GS DT1 message.
static std::vector<uint8_t> makeSysExGS(const std::vector<uint8_t>& data,
                                        uint8_t addrH, uint8_t addrM, uint8_t addrL)
{
    std::vector<uint8_t> body;
    body.push_back(addrH);
    body.push_back(addrM);
    body.push_back(addrL);
    body.insert(body.end(), data.begin(), data.end());

    std::vector<uint8_t> sx;
    sx.push_back(0xF0);
    sx.push_back(0x41); sx.push_back(0x10); sx.push_back(0x42); sx.push_back(0x12);
    sx.insert(sx.end(), body.begin(), body.end());
    sx.push_back(gsChecksum(body));
    sx.push_back(0xF7);
    return sx;
}

// Each byte becomes two 4-bit values, high nibble first.
static std::vector<uint8_t> nibblize(const uint8_t* p, size_t n)
{
    std::vector<uint8_t> out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back((p[i] >> 4) & 0x0F);
        out.push_back(p[i] & 0x0F);
    }
    return out;
}

static std::vector<std::vector<uint8_t>> convertGsdToSysEx(const QByteArray& gsd)
{
    std::vector<std::vector<uint8_t>> out;
    if (gsd.size() < 0x0A71) return out;
    if (!gsd.startsWith("COME ON MUSIC")) return out;
    if (gsd.mid(0x0E, 10) != QByteArray("GS CONTROL")) return out;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(gsd.constData());

    // GS reset first, which is what the sequencer did before pouring a control
    // file into the module. The reference .MID leaves it out because the
    // hardware had already been reset by the time it was recorded; here the
    // module is whatever the last song left behind, and a part that some
    // earlier file switched to rhythm - or muted - stays that way through
    // everything below. Without it the setup lands on top of stale state.
    out.push_back(makeSysExGS({ 0x00 }, 0x40, 0x00, 0x7F));

    // Master tune (4 bytes), then volume / key shift / pan one byte each.
    out.push_back(makeSysExGS({ b[0x20], b[0x21], b[0x22], b[0x23] }, 0x40, 0x00, 0x00));
    for (int i = 0; i < 3; ++i)
        out.push_back(makeSysExGS({ b[0x24 + i] }, 0x40, 0x00, (uint8_t)(0x04 + i)));

    // Reverb (7) and chorus (8), each parameter its own message.
    for (int i = 0; i < 7; ++i)
        out.push_back(makeSysExGS({ b[0x27 + i] }, 0x40, 0x01, (uint8_t)(0x30 + i)));
    for (int i = 0; i < 8; ++i)
        out.push_back(makeSysExGS({ b[0x2E + i] }, 0x40, 0x01, (uint8_t)(0x38 + i)));

    // Voice reserve: how the 24-voice pool is shared out, one byte per part,
    // from each part block's byte 0x00.
    //
    // The order is the part it was never possible to settle from one song. It
    // is the file's own order with **indices 0 and 10 exchanged**, and that is
    // measured rather than reasoned: brute-forcing every one of the block's 122
    // byte offsets against three candidate orderings, over both songs that ship
    // a reference .MID, leaves exactly one answer - offset 0x00, swapped. File
    // order alone fits BAMBOO only because both of its swapped entries are
    // zero; on 3DAYS it would hand the drum part **nothing**, which is why this
    // was left out until now rather than shipped as a guess.
    {
        uint8_t reserve[16] = { 0 };
        for (int i = 0; i < 16; ++i) {
            const size_t at = 0x0035 + (size_t)i * 0x7A;
            if (at < (size_t)gsd.size()) reserve[i] = b[at];
        }
        std::swap(reserve[0], reserve[10]);
        out.push_back(makeSysExGS(std::vector<uint8_t>(reserve, reserve + 16),
                                  0x40, 0x01, 0x10));
    }

    // Sixteen part blocks, 112 bytes each, dumped to the `48 ...` addresses.
    //
    // The file block and the module block are NOT the same thing: the values
    // agree, the layout does not. Copying the file's 0x7A bytes straight in put
    // 66 to 71 of every 128 on the wrong parameter - receive channel,
    // receive-note flags, key ranges - and the SC-55 answered with an error on
    // its display and silence.
    //
    // The layout below was solved against the reference .MID and then checked
    // the only way worth checking: rebuild all 32 part blocks of both sample
    // songs from the .GSD alone and compare. **All 32 come out byte for byte.**
    // Most of the block is fixed - neutral 0x40 and 0x00 defaults - so the
    // template carries those and only the fields that come from the file are
    // written over them.
    static const uint8_t kBlockTemplate[0x70] = {
        // 0x00-0x17 are all overwritten below
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0x00,0x00,0x40,0x40, 0x40,0x40,0x40,0x40,   // 0x18
        0x40,0x40,0x40,0x40, 0x40,0x40,0x10,0x11,   // 0x20
        0x40,0x40,0x40,0x00, 0x40,0x0A,0x00,0x00,   // 0x28
        0x40,0x00,0x00,0x00, 0x00,0x40,0x40,0x00,   // 0x30
        0x40,0x00,0x00,0x00, 0x40,0x00,0x00,0x00,   // 0x38
        0x40,0x40,0x40,0x40, 0x40,0x00,0x00,0x00,   // 0x40
        0x40,0x00,0x00,0x00, 0x40,0x40,0x40,0x40,   // 0x48
        0x40,0x00,0x00,0x00, 0x40,0x00,0x00,0x00,   // 0x50
        0x40,0x40,0x40,0x00, 0x40,0x00,0x00,0x00,   // 0x58
        0x40,0x00,0x00,0x00, 0x40,0x40,0x40,0x00,   // 0x60
        0x40,0x00,0x00,0x00, 0x40,0x00,0x00,0x00,   // 0x68
    };

    static const int kPartOrder[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 14, 15 };

    for (int i = 0; i < 16; ++i) {
        const size_t index = 0x0035 + (size_t)i * 0x7A;   // block base in the file
        if (index + 0x7A > (size_t)gsd.size()) break;
        const uint8_t* f = b + index;

        uint8_t blk[0x70];
        memcpy(blk, kBlockTemplate, sizeof(blk));

        blk[0x00] = f[0x01];              // tone bank
        blk[0x01] = f[0x02];              // tone number
        blk[0x02] = 0xFF;
        blk[0x03] = 0xFF;
        blk[0x04] = f[0x03];              // receive channel
        blk[0x05] = f[0x16] ? 0xB0 : 0x81;  // rhythm part or not
        blk[0x06] = f[0x17];              // key shift
        blk[0x07] = 0x80;
        blk[0x08] = f[0x1A];              // level
        blk[0x09] = f[0x1D];              // panpot
        blk[0x0A] = f[0x1B];
        blk[0x0B] = f[0x1C];
        blk[0x0C] = f[0x19];
        blk[0x0D] = f[0x1F];
        for (int k = 0; k < 10; ++k) blk[0x0E + k] = f[0x22 + k];   // chorus, reverb, tone modify
        blk[0x26] = f[0x20];
        blk[0x27] = f[0x21];
        for (int k = 0; k < 10; ++k) blk[0x2D + k] = f[0x3C + k];   // scale tuning

        const int addr = 0x90 + 0xE0 * kPartOrder[i];
        const std::vector<uint8_t> nib = nibblize(blk, sizeof(blk));
        for (int half = 0; half < 2; ++half) {
            const size_t from = (size_t)half * 128;
            if (from >= nib.size()) break;
            const size_t to = qMin<size_t>(from + 128, nib.size());
            out.push_back(makeSysExGS(std::vector<uint8_t>(nib.begin() + from,
                                                           nib.begin() + to),
                                      0x48,
                                      (uint8_t)((addr >> 7) + half),
                                      (uint8_t)(addr & 0x7F)));
        }
    }

    // The two drum maps.
    //
    // The .GSD carries four bytes per drum note - what the reference sends as
    // the `49 ...` bulk dumps - and leaving them out is why 3DAYS.RCP played
    // its drums louder than the .MID beside it: without them every note in the
    // kit sits at the module's default level instead of the ones the song
    // chose, which run from 79 to 127 here. 6PONGI4.RCP names no control file,
    // which is why that song sounded the same either way and this went unheard
    // for so long.
    //
    // Layout, solved against both reference .MIDs and then verified by
    // rebuilding all sixteen blocks of each from the .GSD alone - 32 of 32
    // byte for byte:
    //
    //   * a map is 83 notes starting at note 27, four bytes to a note;
    //   * map 1 begins at 0x07D6 and map 2 at 0x0922;
    //   * each of the four columns goes out as TWO messages, notes 0-63 and
    //     notes 64-127, and the second one is where the earlier attempt came
    //     unstuck: reading it as a block in its own right makes its first value
    //     look like note 0 when it is note 64.
    //
    // The block numbers are 02/03, 06/07, 08/09, 0A/0B for the first map and
    // the same plus 0x10 for the second.
    {
        static const int kMapBase[2]   = { 0x07D6, 0x0922 };
        static const uint8_t kBlock[4][2] = { {0x02,0x03}, {0x06,0x07},
                                              {0x08,0x09}, {0x0A,0x0B} };
        const int total = (int)gsd.size();
        for (int map = 0; map < 2; ++map) {
            for (int col = 0; col < 4; ++col) {
                uint8_t note[128] = { 0 };
                for (int k = 0; k < 83; ++k) {
                    const int n = 27 + k;
                    const int off = kMapBase[map] + k * 4 + col;
                    if (n < 128 && off < total) note[n] = b[off];
                }
                for (int half = 0; half < 2; ++half) {
                    const std::vector<uint8_t> nib = nibblize(note + half * 64, 64);
                    out.push_back(makeSysExGS(nib, 0x49,
                                              (uint8_t)((map * 0x10) | kBlock[col][half]),
                                              0x00));
                }
            }
        }
    }

    return out;
}

// Which part is the rhythm part, said again in the short form.
//
// It is already in the bulk dump, at byte 0x16 of the block - but only an SC-55
// unpacks a nibblised bulk dump, and both sample songs put their drums on
// channel 2 rather than 10 (3DAYS goes as far as swapping blocks 1 and 9 to do
// it). Sent like this, `40 1n 15`, the player can read it and tell the
// SoundFont engine, which otherwise believes GM and plays a whole drum track as
// a pitched instrument. It goes out early, ahead of the part program changes.
static std::vector<std::vector<uint8_t>> gsdRhythmSysEx(const QByteArray& gsd)
{
    std::vector<std::vector<uint8_t>> out;
    if (gsd.size() < 0x0A71 || !gsd.startsWith("COME ON MUSIC")) return out;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(gsd.constData());
    for (int i = 0; i < 16; ++i) {
        const size_t index = 0x0036 + (size_t)i * 0x7A;
        if (index + 0x7A > (size_t)gsd.size()) break;
        const uint8_t* blk = b + index - 1;      // part block base is 0x0035
        const uint8_t rhythm = blk[0x16];
        if (!rhythm) continue;
        const int ch = blk[0x03];
        if (ch > 15) continue;
        const int slot = (ch == 9) ? 0 : (ch < 9 ? ch + 1 : ch);
        out.push_back(makeSysExGS({ rhythm }, 0x40, (uint8_t)(0x10 | slot), 0x15));
    }
    return out;
}

// The sixteen part blocks again, this time as ordinary channel messages.
//
// The reference .MID beside BAMBOO.RCP sends both: the bulk dump above and,
// for every part, bank / program / volume / pan / reverb / chorus and a pitch
// bend range RPN. Only an SC-55 understands the dump, so without these a
// SoundFont or an MT-32 played the song at default volume, dead centre, with
// no send levels at all.
//
// The offsets were measured, not guessed: each field was matched against what
// the reference sends for all sixteen parts of both sample songs. The block
// index is not the MIDI channel - 3DAYS.GSD has blocks 1 and 9 driving each
// other's channel - so byte 3 of the block, which names the channel it feeds,
// is what routes it.
static std::vector<std::vector<uint8_t>> gsdPartControls(const QByteArray& gsd)
{
    std::vector<std::vector<uint8_t>> out;
    if (gsd.size() < 0x0A71) return out;
    if (!gsd.startsWith("COME ON MUSIC")) return out;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(gsd.constData());

    const size_t kPartBase   = 0x0035;
    const size_t kPartStride = 0x7A;
    const size_t kToneBank   = 0x01;
    const size_t kToneNumber = 0x02;
    const size_t kRxChannel  = 0x03;
    const size_t kLevel      = 0x1A;
    const size_t kPan        = 0x1D;
    const size_t kChorus     = 0x22;
    const size_t kReverb     = 0x23;
    const size_t kBendRange  = 0x43;   // stored biased by 64

    for (int i = 0; i < 16; ++i) {
        const size_t base = kPartBase + (size_t)i * kPartStride;
        if (base + kPartStride > (size_t)gsd.size()) break;

        const uint8_t ch = b[base + kRxChannel];
        if (ch > 15) continue;
        const uint8_t st = (uint8_t)(0xB0 | ch);

        const uint8_t bend = (b[base + kBendRange] >= 64)
                           ? (uint8_t)(b[base + kBendRange] - 64) : 0;

        // Master fine and coarse tune, both neutral. The reference sends
        // them for every part of both songs and the value is 64 every time,
        // which is the no-op, so there is nothing in the .GSD to read them
        // from that these samples could distinguish. A module that speaks GS
        // gets the real figure from the bulk dump above either way; this is
        // for the ones that do not.
        out.push_back({ st, 101, 0 });                       // RPN 0,1 fine
        out.push_back({ st, 100, 1 });
        out.push_back({ st, 6,   64 });
        out.push_back({ st, 38,  0 });
        out.push_back({ st, 101, 0 });                       // RPN 0,2 coarse
        out.push_back({ st, 100, 2 });
        out.push_back({ st, 6,   64 });

        // Bank select MSB is part-block byte 0x01, NOT a constant 0 - and not
        // byte 0x00 either, which is the voice reserve. 3DAYS' part 4 carries
        // 8 there and the reference sends CC0=8: a variation tone, so sending
        // 0 played the whole part on a different instrument. Both other sample
        // songs carry 0 on every part, which is why a hardcoded 0 looked right
        // for as long as it did. The bulk dump above has always read this byte
        // (file 0x01 -> module 0x00, "tone bank"); only this path did not.
        out.push_back({ st, 0,   b[base + kToneBank] });     // bank select MSB
        out.push_back({ st, 32,  0 });                       // bank select LSB
        out.push_back({ (uint8_t)(0xC0 | ch), b[base + kToneNumber], 0 });
        out.push_back({ st, 7,   b[base + kLevel]  });
        out.push_back({ st, 101, 0 });                       // RPN 0,0
        out.push_back({ st, 100, 0 });
        out.push_back({ st, 6,   bend });
        out.push_back({ st, 38,  0 });
        out.push_back({ st, 91,  b[base + kReverb] });
        out.push_back({ st, 93,  b[base + kChorus] });
        out.push_back({ st, 10,  b[base + kPan]    });
    }
    return out;
}

// Reads the .GSD / .CM6 the song names in its header, from beside the song.
// Only GSD is converted: the CM6 (MT-32 / CM-64) path needs its own address
// map and no sample here uses one, so it is left rather than guessed at.
static std::vector<std::vector<uint8_t>> loadControlFileSysEx(const QString& rcpPath,
                                                              const QByteArray& rcpData,
                                                              std::vector<std::vector<uint8_t>>* partControls,
                                                              std::vector<std::vector<uint8_t>>* rhythmSysEx)
{
    std::vector<std::vector<uint8_t>> out;
    if (rcpPath.isEmpty() || rcpData.size() < 0x1E6) return out;

    QByteArray name = rcpData.mid(0x1D6, 16);
    const int nul = name.indexOf('\0');
    if (nul >= 0) name = name.left(nul);
    name = name.trimmed();
    if (name.isEmpty()) return out;

    const QFileInfo fi(rcpPath);
    QString path = fi.absolutePath() + "/" + QString::fromLatin1(name);
    if (!QFile::exists(path)) {
        // The header's spelling and the file on disk often differ in case.
        const QString want = QString::fromLatin1(name).toLower();
        const QFileInfoList sibs = QDir(fi.absolutePath()).entryInfoList(QDir::Files);
        path.clear();
        for (const QFileInfo& s2 : sibs) {
            if (s2.fileName().toLower() == want) { path = s2.absoluteFilePath(); break; }
        }
        if (path.isEmpty()) {
            qWarning() << "[RCP] control file named but not found:" << name;
            return out;
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray ctrl = f.readAll();
    f.close();

    out = convertGsdToSysEx(ctrl);
    if (partControls) *partControls = gsdPartControls(ctrl);
    if (rhythmSysEx) *rhythmSysEx = gsdRhythmSysEx(ctrl);
    qDebug() << "[RCP] control file" << QFileInfo(path).fileName()
             << "->" << (int)out.size() << "SysEx messages,"
             << (partControls ? (int)partControls->size() : 0) << "part controls";
    return out;
}

struct RcpMidiEvent {
    uint32_t tick;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    std::vector<uint8_t> sysex;
    bool isMeta;
    uint8_t metaType;
    std::vector<uint8_t> metaData;

    // Tick, then note-offs ahead of everything else; the sort that uses this
    // must be STABLE - see the note at the call site.
    //
    // A note-off is pushed when its note STARTS, so a stable sort would place
    // it by where it was pushed rather than where it belongs, and it could then
    // land behind a program change that shares its tick. Releasing a note after
    // the part has been switched gives it the new patch's release, which on a
    // sustained sound is audible. Everything else keeps the order the track
    // wrote it in.
    int sortRank() const {
        if (isMeta) return 1;
        const int kind = status & 0xF0;
        if (kind == 0x80 || (kind == 0x90 && data2 == 0)) return 0;
        return 1;
    }
    bool operator<(const RcpMidiEvent& other) const {
        if (tick != other.tick) return tick < other.tick;
        return sortRank() < other.sortRank();
    }
};

struct MeasureInfo {
    uint32_t startTick;
    uint32_t length;
    size_t eventStartIndex;
    size_t eventEndIndex;
    const uint8_t* startByte;   // where in the track this measure begins
};

std::vector<RcpMidiEvent> parseRcpTrack(
    const uint8_t* trackData, size_t trackLen,
    int trackIndex, bool isG36,
    const std::vector<std::vector<uint8_t>>& userSysEx,
    uint8_t defaultMidiCh, uint8_t baseTempo, uint32_t /*measureTicks*/,
    int globalKeyShift)
{
    std::vector<RcpMidiEvent> events;
    if (!trackData || trackLen < 4) return events;

    // A .RCP track header is 44 bytes; only the R36/G36 variant uses 48. This
    // read 48 for both, so the first event of every track was thrown away.
    // What that costs depends on what the event happens to be: BAMBOO.RCP's
    // KEYBOARDS track opens with a 192-tick rest and its ECHO twin with a
    // 200-tick one, which is exactly the head start each of them had over our
    // conversion in the reference .MID - including the 8-tick echo offset that
    // makes the twin an echo at all. Tracks whose first event is a Roland
    // device-number command (step 0) lost no time, which is why only some
    // tracks looked wrong.
    size_t headerSize = isG36 ? 48 : 44;
    if (trackLen <= headerSize) return events;

    // Track name, 36 bytes at offset 8.
    std::string trkName;
    for (int i = 0; i < 36; ++i) {
        char c = static_cast<char>(trackData[8 + i]);
        if (c != 0) trkName += c;
    }
    while (!trkName.empty() && trkName.back() == ' ') trkName.pop_back();

    // Byte 7 bit 0 MUTES the track, and it was being played.
    //
    // The sequencer lets you silence a track and saves that with the song, so a
    // muted track's notes are still in the file - hundreds of them. DEFEAT,
    // KIMI_SC and YAH_GS each carry one (541, 642 and 202 notes) and the
    // reference .MID emits **not one of them**, which is what "we produce more
    // notes than the reference" turned out to be on those songs.
    //
    // Only bit 0. Across the corpus, byte 7 also takes 2, 4, 32 and 56 on 68
    // tracks in 15 songs - and every one of those IS in the reference, so the
    // other bits are something else and must not silence anything.
    if (trackData[7] & 0x01) return events;

    // Byte 4 is MIDI Channel (0..15)
    uint8_t rawCh = trackData[4];
    int midiCh = (rawCh < 16) ? rawCh : (defaultMidiCh % 16);

    // Name-based drum detection is kept - a drum track must not be
    // transposed by the track key shift, and note numbers there are kit slots
    // rather than pitches. What it must NOT do any more is move the channel.
    std::string upperName = trkName;
    std::transform(upperName.begin(), upperName.end(), upperName.begin(), ::toupper);
    const bool isDrumTrack = (upperName.find("DRUM") != std::string::npos ||
                              upperName.find("PERC") != std::string::npos);

    // The track keeps the channel the file gives it. A DRUM/PERC track used to
    // be forced onto channel 10 here "for compatibility with GM/GS
    // soundfonts", which was a workaround for the control file being dropped:
    // an SC-55 song declares its own rhythm parts in the .GSD, and the
    // reference .MID beside BAMBOO.RCP leaves 2,594 drum notes on channel 2
    // because of it. With the GSD now converted and sent, moving them would be
    // the thing that makes playback differ from the original.

    // Byte 5 is the track key shift, and it wraps: Recomposer stores a
    // negative shift as 256 - n, so 116 means -12 and 0x80 means "none".
    //
    // This used to cast the raw byte to int8_t - 116 stays 116 - and then
    // threw it away for being outside +-24, so a track asking for -12 got 0.
    // BAMBOO.RCP's BASS track carries exactly that, which is why its notes
    // sat an octave above the reference .MID.
    // Bit 7 means "no key shift" - the WHOLE byte, not just the value 0x80.
    //
    // `>= 64 ? -128` gets 0x80 right by luck and everything else above it
    // wrong: KIBOU's "Synthesizer 9" carries 140 (0x8C) and came out **twelve
    // semitones sharp** on its channel while the other eleven matched exactly,
    // and KISS_GS carries 129 and came out one semitone sharp. Both are bit 7
    // plus a leftover low value the sequencer does not use.
    //
    // 64..127 really are -64..-1 (BAMBOO's BASS track is 116 = -12), so only
    // the top bit changes meaning.
    int keyShift = trackData[5];
    if (keyShift & 0x80)      keyShift = 0;
    else if (keyShift >= 64)  keyShift -= 128;   // 116 -> -12
    if (keyShift > 24 || keyShift < -24) keyShift = 0;

    // The SONG also carries a key shift, at header offset 0x1C5, and it was
    // ignored entirely. Signed: CROSS_4.RCP holds 0xF4 = -12 and our notes came
    // out an octave above the reference on every melodic channel; LOVE.RCP
    // holds +2 and ours came out two semitones under. Ticks and velocities
    // were already identical in both, which is what pointed at a transpose
    // rather than a parse fault - and channel 10 matched exactly, because
    // drums must not be shifted.
    //
    // Measured over the 303-file corpus: **all 200 songs that already matched
    // carry 0 here**, and 7 of the 103 that did not carry a value. No song can
    // be made worse by honouring it.
    keyShift += globalKeyShift;

    if (!trkName.empty()) {
        QString qTrkName = decodeRcpText(QByteArray(trkName.c_str(), static_cast<int>(trkName.length()))).trimmed();
        if (!qTrkName.isEmpty()) {
            QByteArray utf8Name = qTrkName.toUtf8();
            RcpMidiEvent nameEv;
            nameEv.tick = 0;
            nameEv.isMeta = true;
            nameEv.metaType = 0x03; // Track Name
            nameEv.metaData.assign(utf8Name.begin(), utf8Name.end());
            events.push_back(nameEv);
        }
    }

    const uint8_t* p = trackData + headerSize;
    const uint8_t* end = trackData + trackLen;

    // 0xDF names the device and model, 0xDD the top two bytes of a Roland
    // address and 0xDE the last byte plus the value. Together they are one
    // GS parameter write: BAMBOO.RCP opens with 40 00 04 7F, which is exactly
    // the F0 41 10 42 12 40 00 04 7F 3D F7 the reference .MID sends. These
    // used to fall through to the default case and vanish, losing 156 of the
    // song's 211 SysEx messages.
    uint8_t rolandDev   = 0x10;
    uint8_t rolandModel = 0x42;
    uint8_t rolandAddrH = 0x40;
    uint8_t rolandAddrM = 0x00;

    // Byte 6 of the track header nudges the whole track a few ticks late.
    //
    // Without it the drums of 6PONGI4 played three ticks ahead of everything
    // else for the entire song, and four other tracks one or two - which is
    // what "the drums and the melody are playing separately" was. The header
    // byte matches the measured drift on every one of the thirteen channels:
    // 0 where they lined up, 1 and 2 on the tracks that were a tick or two out,
    // 3 on the drums.
    //
    // Both songs with a reference .MID carry zero here on every track, so this
    // was invisible until a third song turned up.
    uint32_t currentTick = static_cast<uint32_t>(
        static_cast<int8_t>(trackData[6]) > 0 ? static_cast<int8_t>(trackData[6]) : 0);
    std::vector<MeasureInfo> measures;
    uint32_t currentMeasureStartTick = 0;
    size_t currentMeasureStartEv = 0;
    const uint8_t* currentMeasureStartByte = trackData + (isG36 ? 48 : 44);

    // Recomposer ties a note into the following one of the same pitch by
    // giving it a gate longer than its step - 3DAYS.RCP writes step 192 with
    // gate 193, one tick into the next note. The original player does not
    // re-strike in that case, it keeps the note that is already sounding and
    // lets the last link of the chain decide where it ends, so a run of tied
    // events is one long note rather than several short ones.
    int      pendingOffIdx[128];
    uint32_t pendingOffTick[128];
    for (int i = 0; i < 128; ++i) { pendingOffIdx[i] = -1; pendingOffTick[i] = 0; }

    // Where to come back to when a repeated measure (0xFC) or a loop (0xF8)
    // has finished. Both are bounded so a file that points at itself cannot
    // spin: a measure nests 32 deep at most and the whole track is capped at a
    // quarter of a million events, which is far more than any real song.
    // Where a measure replay returns to, and **the loop-stack depth it was
    // entered at**. Without the depth, an 0xF9 inside a replayed measure whose
    // 0xF8 lies outside it pushes a frame that is never popped, once per
    // replay. IKDT2_20.RCP is the extreme: 11 to 14 loop pairs interleaved with
    // 8 to 19 measure repeats per track, and the leak turned a 15 KB file into
    // **574,168 note-ons against the reference's 3,989**.
    struct MeasureReturn { const uint8_t* resume; size_t loopDepth; };
    std::vector<MeasureReturn> measureReturn;
    // Every 0xFC taken since the last 0xFD - a chain. Used only to refuse a
    // repeat that would revisit itself; see the 0xFC handler.
    std::vector<const uint8_t*> fcChain;
    // The byte a replay most recently jumped TO. A 0xFC sitting exactly there
    // is a bar that is itself a copy (tail call); one met further in is the
    // NEXT bar starting, and ends the replay.
    const uint8_t* replayEntry = nullptr;
    struct LoopFrame { const uint8_t* body; int remaining; };
    std::vector<LoopFrame> loopStack;
    size_t stepGuard = 0;

    while (p + 4 <= end) {
        if (++stepGuard > 250000) break;
        uint8_t cmd  = p[0];
        uint8_t step = p[1];
        uint8_t gate = p[2];
        uint8_t vel  = p[3];
        p += 4;

        if (cmd == 0xFE) {
            // End of Track
            break;
        }

        if (cmd <= 0x7F) {
            // Note Event: cmd = Note Number, step = Step Time, gate = Gate Time, vel = Velocity
            // MIDI channel 10 is never transposed, whatever the track is
            // called. Drum-ness was decided by the track NAME alone, and
            // CROSS_4.RCP has a channel-10 track the name rules do not catch:
            // once the song key shift started being honoured, its kit slid down
            // an octave while the other eleven channels came out exactly right.
            const bool noTranspose = isDrumTrack || (midiCh == 9);
            int noteNum = cmd + (noTranspose ? 0 : keyShift);
            if (noteNum < 0) noteNum = 0;
            if (noteNum > 127) noteNum = 127;

            if (gate > 0 && vel > 0) {
                const uint32_t offTick = currentTick + gate;
                const int prev = pendingOffIdx[noteNum];

                if (prev >= 0 && pendingOffTick[noteNum] > currentTick) {
                    // Still sounding: this is a tie. Push the pending note off
                    // out to the new end instead of emitting a second strike.
                    events[static_cast<size_t>(prev)].tick = offTick;
                    pendingOffTick[noteNum] = offTick;
                } else {
                    RcpMidiEvent noteOn;
                    noteOn.tick = currentTick;
                    noteOn.status = static_cast<uint8_t>(0x90 | midiCh);
                    noteOn.data1 = static_cast<uint8_t>(noteNum);
                    noteOn.data2 = vel;
                    noteOn.isMeta = false;
                    events.push_back(noteOn);

                    RcpMidiEvent noteOff;
                    noteOff.tick = offTick;
                    noteOff.status = static_cast<uint8_t>(0x80 | midiCh);
                    noteOff.data1 = static_cast<uint8_t>(noteNum);
                    noteOff.data2 = 0;
                    noteOff.isMeta = false;
                    events.push_back(noteOff);

                    pendingOffIdx[noteNum]  = static_cast<int>(events.size()) - 1;
                    pendingOffTick[noteNum] = offTick;
                }
            }
            currentTick += step;
        } else {
            // Commands
            switch (cmd) {
                case 0x80: case 0x81: case 0x82: case 0x83:
                case 0x84: case 0x85: case 0x86: case 0x87: {
                    int exIdx = cmd - 0x80;
                    if (exIdx < static_cast<int>(userSysEx.size()) && !userSysEx[exIdx].empty()) {
                        std::vector<uint8_t> sx = userSysEx[exIdx];
                        if (sx.size() >= 2 && sx.front() == 0xF0 && sx.back() == 0xF7) {
                            RcpMidiEvent ev;
                            ev.tick = currentTick;
                            ev.status = 0xF0;
                            ev.data1 = ev.data2 = 0;
                            ev.isMeta = false;
                            ev.sysex = sx;
                            events.push_back(ev);
                        }
                    }
                    currentTick += step;
                    break;
                }
                case 0x90: case 0x91: case 0x92: case 0x93:
                case 0x94: case 0x95: case 0x96: case 0x97: {
                    int exIdx = cmd - 0x90;
                    if (exIdx < static_cast<int>(userSysEx.size()) && !userSysEx[exIdx].empty()) {
                        std::vector<uint8_t> sx = userSysEx[exIdx];
                        for (size_t k = 0; k < sx.size(); ++k) {
                            if (sx[k] == 0x80) sx[k] = gate;
                            else if (sx[k] == 0x81) sx[k] = vel;
                        }
                        if (sx.size() >= 2 && sx.front() == 0xF0 && sx.back() == 0xF7) {
                            RcpMidiEvent ev;
                            ev.tick = currentTick;
                            ev.status = 0xF0;
                            ev.data1 = ev.data2 = 0;
                            ev.isMeta = false;
                            ev.sysex = sx;
                            events.push_back(ev);
                        }
                    }
                    currentTick += step;
                    break;
                }
                case 0xFC: {
                    // Same Measure - play a measure from earlier in the track
                    // again.
                    //
                    // **0x98 used to be handled here too, and it is not this
                    // command.** Nothing justified the alias: 3DAYS.RCP and
                    // BAMBOO.RCP contain no 0x98 at all and 6PONGI4.RCP has
                    // exactly one, whose operands are zero so it never took the
                    // pointer path. Across the 213 Recomposer files in the
                    // library 169 carry one, and on YUNO2488.RCP the alias was
                    // fatal: its 0x98 operands are plainly not addresses (0x0115
                    // and 0x0022 are neither 4-byte aligned nor past the header,
                    // where every genuine 0xFC pointer in the same track -
                    // 0x0348, 0x039C, 0x0514 - is both), yet two of them pass
                    // the validity test by luck and jump into the middle of the
                    // stream. The result is a closed cycle between 0x120 and
                    // 0x210 through a run of 0xEB control changes: that one song
                    // converted to 30,000-61,000 CCs per track and **not one
                    // note**, out of the 763 the file actually holds.
                    //
                    // What 0x98 really is has not been established - it is left
                    // to `default:` (advance by step) until a file with a
                    // reference .MID uses one.
                    //
                    // `step` is a measure number and the other two bytes are a
                    // 16-bit offset, from the start of the track INCLUDING its
                    // 44-byte header, of that measure's first event. This used
                    // the measure number and counted 0xFD markers to find it,
                    // and that is wrong: measured over 6PONGI4.RCP's 130 uses
                    // in its drum track alone, the number matches the measure
                    // the offset names 14 times out of 81. The offset is what
                    // the original sequencer follows.
                    //
                    // Neither of the two songs whose reference .MID we can
                    // check uses this command at all, which is how a guess
                    // survived: 3DAYS.RCP and BAMBOO.RCP contain no 0xFC.
                    // 6PONGI4's drum track has 130, and reading the number
                    // instead of the offset left it playing 830 notes where the
                    // reference has 2051.
                    const size_t ptr = static_cast<size_t>(gate) | (static_cast<size_t>(vel) << 8);
                    const uint8_t* target = trackData + ptr;
                    // The pointer is followed wherever it lands. Requiring
                    // it to sit on a measure start was tried - 81 of the drum
                    // track's 130 do - and is wrong: the ones that point into
                    // the middle of a measure are real, and refusing them took
                    // the song from 37,564 ticks to 51,086.
                    if (ptr >= headerSize && target + 4 <= end &&
                        ((target - trackData) % 4) == (headerSize % 4) &&
                        true) {
                        // A repeat met INSIDE a repeat is a TAIL CALL: it jumps
                        // without pushing a second return, so one 0xFC always
                        // plays exactly one bar however long the chain of
                        // "this bar is a copy of that bar" runs.
                        //
                        // This is the shape of the format, not a refinement. A
                        // measure is NOT delimited by the next 0xFD: measure
                        // 0x05A0 of 6PONGI4's channel 2 is fifty consecutive
                        // 0xFC commands with no 0xFD among them, one per bar,
                        // each naming an earlier bar. Ending a replay at the
                        // next 0xFD would replay all fifty as one measure.
                        //
                        // Stepping over a nested one instead - which is what
                        // this used to do - drops its notes AND its time.
                        // Eleven bars of rest collapsed to nothing and the
                        // replay ran straight on into the next bar's notes,
                        // which is where channel 2's 24 spurious notes came
                        // from and why the passage sounded wrong on a seek.
                        // A tail call has no depth limit by design, but it
                        // also has no cycle protection - and IKDT2_20.RCP is a
                        // file where the chain closes on itself. Following it
                        // turned a 15 KB song into **574,168 note-ons against
                        // the reference's 3,989**, running to 5,998,488 ticks
                        // where the reference ends at 16,872. Refusing to take
                        // the SAME 0xFC twice within one chain ends the cycle
                        // without capping legitimate chains: 6PONGI4's fifty
                        // consecutive repeats each name a different bar, so
                        // none of them repeats within its chain.
                        const uint8_t* thisFc = p - 4;
                        // Inside a replay, a 0xFC that is NOT the replayed
                        // measure's own first event ends the measure.
                        //
                        // This reconciles the two things the corpus says at
                        // once. 6PONGI4 has bars that ARE a single 0xFC - fifty
                        // in a row on channel 2, each naming an earlier bar -
                        // so a repeat sitting at the point we jumped to has to
                        // be followed (the tail call). But JOE_INN2's measure 39
                        // is four real notes with **no 0xFD after it**: the next
                        // event is measure 40's repeat. Treating that as a tail
                        // call ran the replay on into the following bar, and
                        // `RCP_TRACE=9` showed the repeat at byte 2004 being
                        // executed FOUR times (ticks 7,680 / 8,064 / 8,640 /
                        // 9,216) where the file holds one.
                        //
                        // So the test is position, not presence: at the entry
                        // byte it is a copy of a copy, past it the next bar has
                        // begun. **219 -> 225 identical, nothing broken**, and
                        // it is what finally settled IKDT2_20 - the file that
                        // was producing 574,168 note-ons.
                        //
                        // Two cruder versions were tried on the corpus first and
                        // both lost songs: ending the replay at ANY 0xFC (219 ->
                        // 196) and ending it after a bar's worth of time from
                        // the time signature (219 -> 218).
                        if (!measureReturn.empty() && thisFc != replayEntry) {
                            p = measureReturn.back().resume;
                            if (loopStack.size() > measureReturn.back().loopDepth)
                                loopStack.resize(measureReturn.back().loopDepth);
                            measureReturn.pop_back();
                            fcChain.clear();
                            replayEntry = nullptr;
                            break;
                        }
                        // RCP_TRACE=<trackIndex> logs every measure repeat this
                        // track takes: where from, where to, and at what tick.
                        // Diagnostic only - the 0xFC rules are the last thing
                        // still costing songs and they cannot be read off the
                        // bytes alone.
                        if (const char* tr = getenv("RCP_TRACE"))
                            if (atoi(tr) == trackIndex)
                                fprintf(stderr, "FC  tick=%6u  from=%5ld  to=%5u  step=%3u\n",
                                        (unsigned)currentTick, (long)(thisFc - trackData),
                                        (unsigned)ptr, (unsigned)step);
                        bool seen = false;
                        for (const uint8_t* v : fcChain) { if (v == thisFc) { seen = true; break; } }
                        if (seen) break;              // cycle - stop following
                        fcChain.push_back(thisFc);
                        replayEntry = target;
                        if (measureReturn.empty())
                            measureReturn.push_back({p, loopStack.size()});   // p is already past this event
                        p = target;
                        continue;                     // the measure carries the time
                    }
                    // An offset that points nowhere falls back on the number.
                    int targetMeasure = step;
                    if (targetMeasure > 0 && targetMeasure <= static_cast<int>(measures.size())) {
                        auto& m = measures[targetMeasure - 1];
                        for (size_t i = m.eventStartIndex; i < m.eventEndIndex && i < events.size(); ++i) {
                            RcpMidiEvent copyEv = events[i];
                            copyEv.tick = currentTick + (copyEv.tick - m.startTick);
                            events.push_back(copyEv);
                        }
                        currentTick += m.length;
                    } else {
                        currentTick += step;
                    }
                    break;
                }
                case 0xF9: {
                    // Loop start. Nothing to emit, and its byte is a marker
                    // rather than a duration - just remember the place.
                    if (loopStack.size() < 16) loopStack.push_back({p, -1});
                    break;
                }
                case 0xF8: {
                    // Loop end. `step` is how many times the block is played in
                    // total, so it jumps back step-1 times.
                    //
                    // Neither of the checkable songs loops either, so this was
                    // never written: the three tracks of 6PONGI4.RCP that use
                    // it simply stopped where their data ran out, a third of
                    // the way through the song.
                    // A loop end met INSIDE a replayed measure must not reach a
                    // loop that was opened OUTSIDE it - taking that frame jumps
                    // the read position out of the measure while the replay is
                    // still pending, and the two then drive each other. Only
                    // frames opened since the replay started are eligible.
                    const size_t floor = measureReturn.empty() ? 0 : measureReturn.back().loopDepth;
                    if (loopStack.size() > floor) {
                        LoopFrame& fr = loopStack.back();
                        if (fr.remaining < 0)
                            fr.remaining = (step > 0) ? (static_cast<int>(step) - 1) : 0;
                        if (fr.remaining > 0) {
                            --fr.remaining;
                            p = fr.body;
                            continue;
                        }
                        loopStack.pop_back();
                    }
                    break;      // the count is not a duration
                }
                case 0xEB: case 0xEA: case 0xD2: {
                    // Control Change: gate = CC#, vel = Value
                    RcpMidiEvent cc;
                    cc.tick = currentTick;
                    cc.status = static_cast<uint8_t>(0xB0 | midiCh);
                    cc.data1 = gate & 0x7F;
                    cc.data2 = vel & 0x7F;
                    cc.isMeta = false;
                    events.push_back(cc);
                    currentTick += step;
                    break;
                }
                case 0xEC: case 0xD3: {
                    // Program Change: gate = Program, vel = Bank
                    if (vel > 0 && !isDrumTrack) {
                        RcpMidiEvent cc0;
                        cc0.tick = currentTick;
                        cc0.status = static_cast<uint8_t>(0xB0 | midiCh);
                        cc0.data1 = 0; // Bank MSB
                        cc0.data2 = vel & 0x7F;
                        cc0.isMeta = false;
                        events.push_back(cc0);
                    }
                    RcpMidiEvent pc;
                    pc.tick = currentTick;
                    pc.status = static_cast<uint8_t>(0xC0 | midiCh);
                    pc.data1 = gate & 0x7F;
                    pc.data2 = 0;
                    pc.isMeta = false;
                    events.push_back(pc);
                    currentTick += step;
                    break;
                }
                case 0xEE: case 0xD0: {
                    // Pitch Bend: gate = LSB, vel = MSB
                    RcpMidiEvent pb;
                    pb.tick = currentTick;
                    pb.status = static_cast<uint8_t>(0xE0 | midiCh);
                    pb.data1 = gate & 0x7F;
                    pb.data2 = vel & 0x7F;
                    pb.isMeta = false;
                    events.push_back(pb);
                    currentTick += step;
                    break;
                }
                case 0xE1: {
                    // Bank Select
                    if (!isDrumTrack) {
                        RcpMidiEvent cc0;
                        cc0.tick = currentTick;
                        cc0.status = static_cast<uint8_t>(0xB0 | midiCh);
                        cc0.data1 = 0;
                        cc0.data2 = vel & 0x7F;
                        cc0.isMeta = false;
                        events.push_back(cc0);
                    }
                    currentTick += step;
                    break;
                }
                case 0xE2: {
                    // Bank select and program change, not tempo.
                    //
                    // 3DAYS.RCP has two of these, and the reference .MID
                    // answers each with CC0 = vel, CC32 = 0 and a program
                    // change of gate on the same channel and tick - 0xE2
                    // gate 0 vel 8 becomes CC0=8, CC32=0, program 0. Reading
                    // it as a tempo change moved the whole song's speed on a
                    // message that was only asking for a variation tone.
                    {
                        RcpMidiEvent bankMsb;
                        bankMsb.tick = currentTick;
                        bankMsb.isMeta = false;
                        bankMsb.status = static_cast<uint8_t>(0xB0 | midiCh);
                        bankMsb.data1 = 0;
                        bankMsb.data2 = vel;
                        events.push_back(bankMsb);

                        RcpMidiEvent bankLsb = bankMsb;
                        bankLsb.data1 = 32;
                        bankLsb.data2 = 0;
                        events.push_back(bankLsb);

                        RcpMidiEvent pc;
                        pc.tick = currentTick;
                        pc.isMeta = false;
                        pc.status = static_cast<uint8_t>(0xC0 | midiCh);
                        pc.data1 = gate;
                        pc.data2 = 0;
                        events.push_back(pc);
                    }
                    currentTick += step;
                    break;
                }
                case 0xE7: {
                    // Tempo change, and it is a RATIO of the song's own tempo
                    // rather than a BPM: 64 means "x1.00".
                    //
                    // 6PONGI4.RCP is the first sample here that carries one. It
                    // sends 64 near the end, and the reference .MID holds 154
                    // BPM straight through - where reading the operand as a BPM
                    // dropped the last bars to 64.
                    const uint32_t ratio = (gate > 0) ? gate : vel;
                    const uint32_t base  = (baseTempo > 0) ? baseTempo : 120;
                    uint32_t bpm = (ratio > 0) ? (base * ratio + 32) / 64 : base;
                    if (bpm >= 20 && bpm <= 300) {
                        uint32_t usPerQuarter = 60000000 / bpm;
                        RcpMidiEvent ev;
                        ev.tick = currentTick;
                        ev.isMeta = true;
                        ev.metaType = 0x51;
                        ev.metaData.push_back(static_cast<uint8_t>((usPerQuarter >> 16) & 0xFF));
                        ev.metaData.push_back(static_cast<uint8_t>((usPerQuarter >> 8) & 0xFF));
                        ev.metaData.push_back(static_cast<uint8_t>(usPerQuarter & 0xFF));
                        events.push_back(ev);
                    }
                    currentTick += step;
                    break;
                }
                case 0xE6: {
                    // Channel Change - the operand is `gate`, and it is
                    // ONE-BASED (1..16), not `vel` and not an index.
                    //
                    // This read `vel`, which is zero in every real file, so
                    // `midiCh` was forced to 0 and **every note in the song
                    // ended up on channel 1**. ALL_GS.RCP is the clean proof:
                    // each of its eight tracks opens with one 0xE6 whose gate
                    // is exactly the header channel plus one (header 9 -> 10,
                    // 0 -> 1, 1 -> 2 ... 6 -> 7), and our conversion piled all
                    // 6,675 of its notes onto channel 1 while the reference
                    // .MID spreads them over eight channels.
                    //
                    // Nothing could show this until the owner was sent 303
                    // .RCP files WITH their reference .MID beside them: the
                    // three songs checked before use no 0xE6 at all, and it
                    // appears in 40% of the files that mismatch against 10% of
                    // the ones that match.
                    if (gate >= 1 && gate <= 16) midiCh = gate - 1;
                    currentTick += step;
                    break;
                }
                case 0xFD: {
                    // Measure End / Bar Mark. It is also where a measure being
                    // replayed by 0xFC finishes and hands back.
                    fcChain.clear();              // a bar ended: chain over
                    if (!measureReturn.empty()) {
                        p = measureReturn.back().resume;
                        // Drop any loop frame opened inside the replayed
                        // measure - see the MeasureReturn note above.
                        if (loopStack.size() > measureReturn.back().loopDepth)
                            loopStack.resize(measureReturn.back().loopDepth);
                        measureReturn.pop_back();
                        replayEntry = nullptr;
                        break;
                    }
                    uint32_t mLen = (currentTick >= currentMeasureStartTick) ? (currentTick - currentMeasureStartTick) : 0;
                    measures.push_back({currentMeasureStartTick, mLen, currentMeasureStartEv, events.size(), currentMeasureStartByte});
                    // A measure end is a marker, not a rest: its step byte is
                    // not time. Adding it inserted one spurious tick per bar -
                    // 966 of them in BAMBOO.RCP - so the music drifted late by
                    // a tick per measure against the reference .MID, which is
                    // why the same passage measured 97 ticks here and 96 there.
                    currentMeasureStartTick = currentTick;
                    currentMeasureStartEv = events.size();
                    currentMeasureStartByte = p;
                    break;
                }
                case 0xDF: {
                    rolandDev   = gate;
                    rolandModel = vel;
                    currentTick += step;
                    break;
                }
                case 0xDD: {
                    rolandAddrH = gate;
                    rolandAddrM = vel;
                    currentTick += step;
                    break;
                }
                case 0xDE: {
                    const std::vector<uint8_t> body = {
                        rolandAddrH, rolandAddrM, gate, vel
                    };
                    std::vector<uint8_t> sx;
                    sx.push_back(0xF0);
                    sx.push_back(0x41);
                    sx.push_back(rolandDev);
                    sx.push_back(rolandModel);
                    sx.push_back(0x12);
                    sx.insert(sx.end(), body.begin(), body.end());
                    sx.push_back(gsChecksum(body));
                    sx.push_back(0xF7);

                    RcpMidiEvent ev;
                    ev.tick = currentTick;
                    ev.isMeta = false;
                    ev.status = 0xF0;
                    ev.sysex = sx;
                    events.push_back(ev);
                    currentTick += step;
                    break;
                }
                case 0xF6: case 0xF7: {
                    // Comment text. All THREE bytes after the command are
                    // characters - none of them is a step - so a comment takes
                    // no time.
                    //
                    // Adding the first one put the drums of 6PONGI4 twelve
                    // ticks behind everything else for the whole song, off one
                    // comment whose first character happens to be 0x0C. Neither
                    // song with a reference .MID can show this: BAMBOO has no
                    // comments at all and all twenty of 3DAYS' start with a
                    // zero byte. 6PONGI4 has 2,353 comments and exactly one
                    // that does not.
                    break;
                }
                case 0xF5: {
                    // Key signature. Its byte is NOT a duration - falling into
                    // `default:` below added it to the clock, and the track ran
                    // that many ticks late from there on, accumulating.
                    //
                    // BT_DAWN.RCP shows it cleanly: a 0xF5 with step 3 sits at
                    // tick 7920 and the very next note lands at 7923 for us
                    // against 7920 in the reference, then again at 8112, 8376,
                    // 8496 - three ticks each time. The note sequence, pitches
                    // and velocities were already identical, which is what said
                    // this was a clock fault and not a parse one. Counted over
                    // the corpus, 0xF5 appears in 11.7% of the files that
                    // mismatched and 0.5% of the ones that matched.
                    break;
                }
                default:
                    currentTick += step;
                    break;
            }
        }
    }

    // STABLE, so events sharing a tick stay in the order the track wrote them.
    //
    // This was `std::sort` with a comparator that pushed every note-on behind
    // everything else at the same tick, and that is not a detail: 3DAYS puts a
    // note-on BEFORE a program change nine times on channel 6, deliberately, so
    // the note sounds with the outgoing instrument while the next note takes
    // the new one. Reordering them made both notes play the new program - the
    // owner heard it as "the choir pad is a different sound in the .RCP".
    // std::sort is not stable either, so the rest of a tick's order was
    // arbitrary and could differ between builds.
    std::stable_sort(events.begin(), events.end());
    return events;
}

} // namespace

// The extension is not enough, and .sng is why.
//
// "3X3EYES2.SNG" is a Ballade file - a different PC-98 driver entirely, which
// says so in its first line ("BALLADE SONG Ver. 4.00"). Trusting the extension
// converted it as Recomposer anyway and produced a playable-looking result:
// 12 tracks, 1298 events and a running time of 231 minutes, with no sound.
// Failing to open it is the honest answer until Ballade is actually read.
bool RcpFileHandler::isRcpFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = file.read(512);
    file.close();
    return isRcpData(header);
}

bool RcpFileHandler::isRcpData(const QByteArray& data)
{
    if (data.size() < 64) return false;

    // Every Recomposer variant names itself in the first line: .RCP opens
    // "RCM-PC98V2.0(C)COME ON MUSIC" and the R36/G36 form carries RECOMPOSER.
    // The old test also accepted a plausible timebase byte at 0x1C0 as
    // evidence, which is one byte out of five values - Ballade passed it.
    const QByteArray head = data.left(64);
    return head.contains("COME ON MUSIC") || head.contains("RCM-PC98") ||
           head.contains("RECOMPOSER")    || head.contains("Recomposer");
}

QString RcpFileHandler::extractTitle(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QByteArray data = file.read(512);
    file.close();
    return extractTitle(data);
}

QString RcpFileHandler::extractTitle(const QByteArray& data)
{
    if (data.size() < 32) return QString();

    // The first 32 bytes are the signature ("RCM-PC98V2.0(C)COME ON MUSIC");
    // the song name is the 64-byte field after it. Reading from 0 returned the
    // signature as the title of every file.
    bool isG36 = data.contains("G36") || data.contains("G18");
    const int titleOff = isG36 ? 0x20 : 0x20;
    const int titleLen = 64;
    if (data.size() < titleOff + 1) return QString();

    QByteArray rawTitle = data.mid(titleOff, qMin<int>(titleLen, data.size() - titleOff));
    int nullIdx = rawTitle.indexOf('\0');
    if (nullIdx >= 0) rawTitle = rawTitle.left(nullIdx);

    QString title = decodeRcpText(rawTitle).trimmed();
    return title;
}

QString RcpFileHandler::extractMemo(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QByteArray data = file.read(1024);
    file.close();
    return extractMemo(data);
}

QString RcpFileHandler::extractMemo(const QByteArray& data)
{
    if (data.size() < 0x1C0) return QString();

    bool isG36 = data.contains("G36") || data.contains("G18");
    int memoOffset = isG36 ? 64 : 32;
    int memoLen = isG36 ? 360 : 336;

    if (data.size() < memoOffset + memoLen) return QString();

    QByteArray rawMemo = data.mid(memoOffset, memoLen);
    return decodeRcpText(rawMemo).trimmed();
}

QByteArray RcpFileHandler::extractMidiData(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[RcpFileHandler] Cannot open file:" << filePath;
        return QByteArray();
    }
    QByteArray data = file.readAll();
    file.close();

    // Refuse anything that is not Recomposer, whatever its extension says.
    // A Ballade .SNG converted into 12 tracks, 1298 events and 231 minutes of
    // silence, which looks far more like a broken player than an unsupported
    // format.
    if (!isRcpData(data)) {
        qWarning() << "[RcpFileHandler] not a Recomposer file:" << filePath
                   << "- first bytes:" << data.left(24);
        return QByteArray();
    }

    // The path matters: the song's setup lives in a separate .GSD beside it.
    return convertRcp(data, filePath);
}

QByteArray RcpFileHandler::extractMidiData(const QByteArray& rcpData)
{
    // No path, so no control file - the caller gets the sequence alone.
    return convertRcp(rcpData, QString());
}

QByteArray RcpFileHandler::convertRcp(const QByteArray& rcpData, const QString& srcPath)
{
    if (rcpData.size() < 0x200) {
        qWarning() << "[RcpFileHandler] RCP file is too small";
        return QByteArray();
    }

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(rcpData.constData());
    size_t totalLen = rcpData.size();

    bool isG36 = rcpData.contains("G36") || rcpData.contains("G18");

    // Standard Recomposer resolution is stored at 0x1C0 (G36 at 0x1E0)
    uint16_t timebase = 48;
    uint8_t initialTempo = 120;

    if (isG36 && totalLen >= 0x1E4) {
        uint16_t tb = readLE16(raw + 0x1E0);
        if (tb == 48 || tb == 96 || tb == 120 || tb == 192 || tb == 240 || tb == 384) {
            timebase = tb;
        }
        initialTempo = raw[0x1E2];
    } else if (totalLen >= 0x1C2) {
        uint8_t tb = raw[0x1C0];
        if (tb == 48 || tb == 96 || tb == 120 || tb == 192 || tb == 240 || tb == 384) {
            timebase = tb;
        } else {
            timebase = 48;
        }
        initialTempo = (raw[0x1C1] > 0) ? raw[0x1C1] : 120;
    }
    if (initialTempo == 0) initialTempo = 120;

    // Song-wide key shift, header offset 0x1C5, signed. See the note in
    // parseRcpTrack where it is folded into the per-track shift.
    int globalKeyShift = 0;
    if (totalLen > 0x1C5) {
        globalKeyShift = static_cast<int8_t>(raw[0x1C5]);
        if (globalKeyShift > 24 || globalKeyShift < -24) globalKeyShift = 0;
    }

    // One bar, in ticks, from the header's time signature - what a measure-end
    // marker rounds up to.
    uint32_t measureTicks = 0;
    {
        const uint8_t num = raw[0x1C2];
        const uint8_t den = raw[0x1C3];
        if (num > 0 && den > 0 && timebase > 0)
            measureTicks = static_cast<uint32_t>(timebase) * 4u * num / den;
    }

    // User SysEx Definitions
    std::vector<std::vector<uint8_t>> userSysEx(8);
    size_t sysexTableOffset = isG36 ? 0x200 : 0x406;
    size_t sysexBlockSize = isG36 ? 24 : 48;

    if (totalLen >= sysexTableOffset + 8 * sysexBlockSize) {
        for (int i = 0; i < 8; ++i) {
            const uint8_t* sxPtr = raw + sysexTableOffset + (i * sysexBlockSize);
            std::vector<uint8_t> sx;
            for (size_t k = 0; k < sysexBlockSize; ++k) {
                sx.push_back(sxPtr[k]);
                if (sxPtr[k] == 0xF7) break;
            }
            if (!sx.empty() && sx.front() == 0xF0) {
                userSysEx[i] = sx;
            }
        }
    }

    int maxTracks = isG36 ? 36 : 18;
    size_t trackPtrOffset = isG36 ? 0x400 : 0x586;

    std::vector<std::vector<RcpMidiEvent>> allTracks;

    // Track 0 (Tempo / Info Track)
    bool hasSetup = false;
    {
        std::vector<RcpMidiEvent> trk0;
        QString title = extractTitle(rcpData);
        if (!title.isEmpty()) {
            QByteArray utf8Title = title.toUtf8();
            RcpMidiEvent nameEv;
            nameEv.tick = 0;
            nameEv.isMeta = true;
            nameEv.metaType = 0x03;
            nameEv.metaData.assign(utf8Title.begin(), utf8Title.end());
            trk0.push_back(nameEv);
        }

        RcpMidiEvent tsEv;
        tsEv.tick = 0;
        tsEv.isMeta = true;
        tsEv.metaType = 0x58;
        tsEv.metaData = {0x04, 0x02, 0x18, 0x08};
        trk0.push_back(tsEv);

        // The module setup, before a single note. The sequencer sent this to
        // the SC-55 as a bulk dump; without it the parts play the wrong
        // patches and nothing declares which channels are rhythm.
        //
        // It is spread across the lead-in rather than piled on tick 0, the way
        // the reference .MID does it, because a real SC-55 - and the emulated
        // one - needs time to swallow a couple of hundred messages, and a
        // burst at tick 0 arrives faster than the module can answer.
        {
            std::vector<std::vector<uint8_t>> parts, rhythm;
            const std::vector<std::vector<uint8_t>> ctrl =
                loadControlFileSysEx(srcPath, rcpData, &parts, &rhythm);
            hasSetup = !ctrl.empty() || !parts.empty();

            // Two cursors, running at the same time, because that is what
            // the reference does and the rate is the point.
            //
            // Measured on BAMBOO.MID: it moves 243 bytes of bulk dump per part
            // across 20 ticks - about 1,300 bytes a second - with the part's
            // controllers filling the gaps. Piling the SysEx into a block of
            // its own and the controllers after it, as this did, came out at
            // 2,600 bytes a second. `sc55bridge.h` records that an unpatched
            // emulator manages "around 1000": that path feeds the sub-MCU's
            // 1024-byte buffer, which has no overflow check, and an opening
            // burst that outruns it is partly overwritten.
            //
            // So SysEx is paced by its own length - about twelve bytes a tick -
            // and the channel messages run alongside at one per tick, three
            // bytes each. Together that stays near the reference's figure and
            // still finishes inside the lead-in.
            const auto pace = [](size_t bytes) -> uint32_t {
                const uint32_t t = (uint32_t)(bytes / 12);
                return t < 1 ? 1u : t;
            };

            uint32_t at = 4;
            bool first = true;
            auto emitSysEx = [&](const std::vector<uint8_t>& sx) {
                RcpMidiEvent ev;
                ev.tick = at;
                ev.isMeta = false;
                ev.status = 0xF0;
                ev.sysex = sx;
                trk0.push_back(ev);
                // A GS reset takes the module a moment to act on, so nothing
                // follows it too closely.
                at += first ? 8 : pace(sx.size());
                first = false;
            };

            for (size_t i = 0; i < ctrl.size(); ++i) {
                emitSysEx(ctrl[i]);
                // The rhythm assignment rides just behind the reset, ahead of
                // every program change, or a part is already melodic by the
                // time it is told otherwise.
                if (i == 0)
                    for (const std::vector<uint8_t>& sx : rhythm) emitSysEx(sx);
            }

            // Three per tick: they are three bytes each, so that is nine
            // bytes a tick against the twelve the SysEx above is paced at.
            uint32_t ctrlAt = 14;
            int perTick = 0;
            for (const std::vector<uint8_t>& m : parts) {
                if (m.size() < 3) continue;
                RcpMidiEvent ev;
                ev.tick = ctrlAt;
                ev.isMeta = false;
                ev.status = m[0];
                ev.data1 = m[1];
                ev.data2 = m[2];
                trk0.push_back(ev);
                if (++perTick == 3) { perTick = 0; ctrlAt += 1; }
            }
            at = (at > ctrlAt) ? at : ctrlAt;
        }

        // The lead-in runs at the default 120, and the song's own tempo takes
        // over on the tick the music starts - which is what the reference does,
        // and why its setup takes six seconds where ours took seven and a half.
        // ...and a song with no setup has its own tempo from tick 0, so the
        // 120 would only be overwritten at the same instant. Skipping it keeps
        // the tempo map the same length as the reference's.
        if (hasSetup) {
            RcpMidiEvent defaultTempo;
            defaultTempo.tick = 0;
            defaultTempo.isMeta = true;
            defaultTempo.metaType = 0x51;
            defaultTempo.metaData = {0x07, 0xA1, 0x20};   // 500000 us = 120 BPM
            trk0.push_back(defaultTempo);
        }

        if (initialTempo != 120) {
            uint32_t usPerQuarter = 60000000 / initialTempo;
            RcpMidiEvent tempoEv;
            tempoEv.tick = hasSetup ? (uint32_t)timebase * 12 : 0;
            tempoEv.isMeta = true;
            tempoEv.metaType = 0x51;
            tempoEv.metaData = {
                static_cast<uint8_t>((usPerQuarter >> 16) & 0xFF),
                static_cast<uint8_t>((usPerQuarter >> 8) & 0xFF),
                static_cast<uint8_t>(usPerQuarter & 0xFF)
            };
            trk0.push_back(tempoEv);
        }

        // Track 0 is built by hand out of pieces written at whatever tick each
        // belongs on, so it has to be put in order before it is written. It
        // was not, and the writer clamps a backwards delta to zero rather than
        // failing - which quietly dragged the tempo events to the end of the
        // setup and made the lead-in play at the wrong speed.
        //
        // STABLE, and that is not a nicety. The setup packs three channel
        // messages to a tick, and an RPN is FOUR messages that only mean
        // anything in order: select the parameter with CC101/CC100, then send
        // its value with CC6/CC38. An unstable sort shuffled them inside a tick
        // and the value arrived before the selector - so `RPN 0,0 = 12`, the
        // pitch bend range, was delivered while the previous group's RPN 0,2
        // was still selected and set COARSE TUNING to 12 instead. That
        // transposes the whole part, and it is what the owner heard as "the
        // .MID's choir pad is bright and ours sounds like an electric guitar":
        // channel 6 of 3DAYS was playing detuned, measured at 27.7 dB of band
        // deviation with 49 dB too much at 40-80 Hz.
        std::stable_sort(trk0.begin(), trk0.end());

        allTracks.push_back(trk0);
    }

    // Parse Music Tracks
    size_t currentPos = trackPtrOffset;
    size_t headerSize = isG36 ? 48 : 44;

    for (int t = 0; t < maxTracks && currentPos + headerSize <= totalLen; ++t) {
        uint16_t trackSize = readLE16(raw + currentPos);
        
        if (trackSize <= headerSize) {
            currentPos += headerSize;
            continue;
        }

        if (currentPos + trackSize > totalLen) {
            trackSize = static_cast<uint16_t>(totalLen - currentPos);
        }

        uint8_t defaultCh = static_cast<uint8_t>(t % 16);
        std::vector<RcpMidiEvent> trkEvents = parseRcpTrack(
            raw + currentPos, trackSize, t, isG36, userSysEx, defaultCh, initialTempo,
            measureTicks, globalKeyShift
        );

        if (!trkEvents.empty()) {
            allTracks.push_back(trkEvents);
        }

        currentPos += trackSize;
    }

    // Every track starts 576 ticks in, which is what the reference .MID does
    // at this timebase: the setup above has to reach the module before the
    // first note, and once it is applied our note stream and the reference's
    // line up tick for tick on every channel of both sample songs.
    // ... but a song that names no control file has nothing to send and so
    // nothing to wait for, and 6 seconds of silence in front of it is just
    // silence. 6PONGI4.RCP is one of those.
    {
        // Twelve beats, the reference's own figure, because the setup is the
        // same size as the reference's again and needs the same time on the
        // wire. A song that names no control file still starts straight away.
        const uint32_t leadIn = hasSetup ? (uint32_t)timebase * 12 : 0;
        bool first = true;
        for (std::vector<RcpMidiEvent>& trk : allTracks) {
            // Track 0 is the conductor and the setup; both were written at the
            // ticks they are meant to play at, so only the music moves.
            if (first) { first = false; continue; }
            for (RcpMidiEvent& ev : trk) ev.tick += leadIn;
        }
    }

    // Assemble Standard MIDI File (SMF Format 1)
    std::vector<uint8_t> smf;

    smf.push_back('M'); smf.push_back('T'); smf.push_back('h'); smf.push_back('d');
    writeBigEndian32(smf, 6);
    writeBigEndian16(smf, 1);
    writeBigEndian16(smf, static_cast<uint16_t>(allTracks.size()));
    writeBigEndian16(smf, timebase);

    for (auto& trk : allTracks) {
        std::vector<uint8_t> trkBody;
        uint32_t lastTick = 0;
        uint8_t runningStatus = 0;

        for (const auto& ev : trk) {
            uint32_t delta = (ev.tick >= lastTick) ? (ev.tick - lastTick) : 0;
            writeVarLen(trkBody, delta);
            lastTick = ev.tick;

            if (ev.isMeta) {
                trkBody.push_back(0xFF);
                trkBody.push_back(ev.metaType);
                writeVarLen(trkBody, static_cast<uint32_t>(ev.metaData.size()));
                trkBody.insert(trkBody.end(), ev.metaData.begin(), ev.metaData.end());
                runningStatus = 0;
            } else if (ev.status == 0xF0 || ev.status == 0xF7) {
                // The builders hand back a complete message, F0 and all, but
                // the status byte is written separately here - so the F0 has
                // to come off or every message carries two of them.
                const bool dup = (!ev.sysex.empty() && ev.sysex.front() == 0xF0
                                  && ev.status == 0xF0);
                const size_t from = dup ? 1u : 0u;
                trkBody.push_back(ev.status);
                writeVarLen(trkBody, static_cast<uint32_t>(ev.sysex.size() - from));
                trkBody.insert(trkBody.end(), ev.sysex.begin() + from, ev.sysex.end());
                runningStatus = 0;
            } else {
                if (ev.status != runningStatus) {
                    trkBody.push_back(ev.status);
                    runningStatus = ev.status;
                }
                trkBody.push_back(ev.data1);
                uint8_t st = ev.status & 0xF0;
                if (st != 0xC0 && st != 0xD0) {
                    trkBody.push_back(ev.data2);
                }
            }
        }

        writeVarLen(trkBody, 0);
        trkBody.push_back(0xFF);
        trkBody.push_back(0x2F);
        trkBody.push_back(0x00);

        smf.push_back('M'); smf.push_back('T'); smf.push_back('r'); smf.push_back('k');
        writeBigEndian32(smf, static_cast<uint32_t>(trkBody.size()));
        smf.insert(smf.end(), trkBody.begin(), trkBody.end());
    }

    qDebug() << "[RcpFileHandler] Converted RCP to SMF1. Tracks:" << allTracks.size() << "Size:" << smf.size() << "Division:" << timebase << "Initial Tempo:" << initialTempo;

    return QByteArray(reinterpret_cast<const char*>(smf.data()), static_cast<int>(smf.size()));
}
