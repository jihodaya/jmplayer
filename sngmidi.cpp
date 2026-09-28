#include "sngmidi.h"

#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cstring>
#include <vector>

#include <windows.h>

namespace {

const int   kTicksPerQuarter = 48;
const char* kSignature       = "BALLADE SONG";

inline uint16_t rd16(const unsigned char* d, int o) { return uint16_t(d[o] | (d[o + 1] << 8)); }
inline uint32_t rd32(const unsigned char* d, int o)
{
    return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) |
           (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
}

// Ballade is a Japanese program; its text is Shift-JIS, always. Same reasoning
// as RcpFileHandler - the player's general decoder tries CP949 first because
// this library is mostly Korean, and full-width Latin is valid in both.
QString decodeSjis(const unsigned char* d, int len)
{
    if (len <= 0) return QString();
    const int n = MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(d), len, nullptr, 0);
    if (n <= 0) return QString();
    std::wstring buf(size_t(n), L'\0');
    if (MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(d), len, &buf[0], n) <= 0)
        return QString();
    return QString::fromWCharArray(buf.data(), n).trimmed();
}

// Ballade's tempo is a slider position, not a BPM. Sweeping every value from 1
// to 127 through SNG2S gives a curve that climbs by two and stalls for one
// every eighth step - 61 is 114, 72 is 135, 91 is 170.
//
// A song in 3/4 measures one BPM higher for the same value, which is SNG2S's
// own rounding rather than anything Ballade does (the driver programs a timer,
// and a timer cannot know the time signature), so the one curve is used for
// every meter.
int bpmForTempoValue(int v)
{
    const int bpm = 2 * v - ((v + 7) / 8);
    return std::max(1, std::min(600, bpm));
}

struct Event {
    int      tick;
    int      order;      // controllers before note-offs before note-ons, per tick
    uint8_t  bytes[3];
    int      len;
};

struct Track {
    QByteArray         name;
    int                channel = 0;
    std::vector<Event> events;
};

void writeVarLen(std::vector<uint8_t>& out, uint32_t v)
{
    uint8_t stack[5];
    int n = 0;
    stack[n++] = uint8_t(v & 0x7F);
    v >>= 7;
    while (v) { stack[n++] = uint8_t((v & 0x7F) | 0x80); v >>= 7; }
    while (n) out.push_back(stack[--n]);
}

void writeBE32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
}

struct Layout {
    int version   = 0;      // 100, 200, 300, 400
    int parts     = 0;
    int headerEnd = 0;      // where block 0 begins
    std::vector<int> blockStart;   // [0] = conductor, [1..parts] = parts
    std::vector<int> blockSize;
};

// The block table is (parts + 1) little-endian 32-bit sizes at 0x46: the
// conductor first, then one per part - each part's block carrying a fixed
// 81-byte heap prefix on top of the size the table gives.
//
// What sits between the table and the first block is a fixed settings area
// whose length is the one thing that changed between versions, so rather than
// trust a table of constants the candidates are tried and the one whose blocks
// actually describe the file wins.
bool buildLayout(const unsigned char* d, int n, Layout& out)
{
    if (n < 0x100 || memcmp(d, kSignature, 12) != 0) return false;

    const QString ver = QString::fromLatin1(reinterpret_cast<const char*>(d) + 18, 4);
    int parts;
    if (ver == "4.00")                                     parts = 16;
    else if (ver == "3.00" || ver == "2.00" || ver == "1.00") parts = 10;
    else return false;

    const int tableEnd = 0x46 + 4 * (parts + 1);
    if (tableEnd + 8 > n) return false;

    std::vector<uint32_t> ent(size_t(parts) + 1);
    for (int i = 0; i <= parts; ++i) ent[size_t(i)] = rd32(d, 0x46 + 4 * i);

    // 94 bytes of settings for Ballade2 onwards, 2 for the oldest files.
    const int candidates[] = { tableEnd + 94, tableEnd + 2, tableEnd };
    for (int H : candidates) {
        if (H <= 0 || H >= n) continue;
        Layout L;
        L.parts = parts;
        L.headerEnd = H;
        L.version = ver.left(1).toInt() * 100;
        long off = H;
        L.blockStart.push_back(int(off));
        L.blockSize.push_back(int(ent[0]));
        off += ent[0];
        bool ok = true;
        for (int k = 1; k <= parts && ok; ++k) {
            const long size = long(ent[size_t(k)]) + 81;
            if (off < 0 || off + 158 > n || size < 158) { ok = false; break; }
            L.blockStart.push_back(int(off));
            L.blockSize.push_back(int(size));
            // A block's own header says how much of it is used; if that does
            // not fit, this candidate is not the right header length.
            const int s = int(off);
            const int used = rd16(d, s + 112), ctl = rd16(d, s + 114), idx = rd16(d, s + 116);
            const long cap = size - 158;
            if ((used % 8) != 0 || (ctl % 4) != 0 ||
                long(used) + ctl + idx > cap) ok = false;
            off += size;
        }
        // A block is allowed to run off the end of the file - MAGICAL0.SNG is
        // cut at a round 126 KB and its rhythm part is half missing - because
        // every part before it still plays and the reader clamps its walk to
        // the bytes that are there. Every block START must be inside the file,
        // which is what makes a wrong header length fail.
        if (ok) { out = L; return true; }
    }
    return false;
}

} // namespace

namespace sngmidi {

bool isSngData(const QByteArray& data)
{
    if (data.size() < 32) return false;
    return memcmp(data.constData(), kSignature, 12) == 0;
}

bool isSngFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return isSngData(f.read(64));
}

QString extractTitle(const QByteArray& data)
{
    if (!isSngData(data)) return QString();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    const int n = data.size();
    // "BALLADE SONG Ver. 4.00 \r\n" then the name, then \r\n\x1a.
    int i = 0;
    while (i + 1 < n && !(d[i] == 0x0D && d[i + 1] == 0x0A)) ++i;
    i += 2;
    int end = i;
    while (end < n && d[end] != 0x0D && d[end] != 0x1A && d[end] != 0x00) ++end;
    if (end <= i) return QString();
    return decodeSjis(d + i, end - i);
}

QString extractTitle(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return extractTitle(f.read(256));
}

QByteArray toMidi(const QByteArray& data)
{
    if (!isSngData(data)) return QByteArray();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    const int n = data.size();

    Layout L;
    if (!buildLayout(d, n, L)) return QByteArray();

    // ---- conductor: a 12-byte record per bar, then the tempo stream.
    int barLen = 192, sigNum = 4, sigDen = 4;
    {
        const int s = L.blockStart[0], e = std::min(n, s + L.blockSize[0]);
        const int o = s + 77;
        if (o + 12 <= e) {
            const int len = rd16(d, o);
            const int num = d[o + 2], den = d[o + 3];
            if (len > 0 && len <= 4000 && num > 0 && num <= 32 &&
                (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32)) {
                barLen = len; sigNum = num; sigDen = den;
            }
        }
    }

    struct TempoPoint { int tick; int us; };
    std::vector<TempoPoint> tempi;
    {
        const int s = L.blockStart[0], e = std::min(n, s + L.blockSize[0]);
        // The heap marks the end of a chunk with 7FFF FFFF FFFF; the tempo
        // stream follows the last one, past six bytes of allocator fields.
        static const unsigned char kSentinel[6] = { 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF };
        int last = -1;
        for (int i = s; i + 6 <= e; ++i)
            if (memcmp(d + i, kSentinel, 6) == 0) last = i;
        if (last >= 0) {
            int o = last + 12, tick = 0, prevUs = -1;
            while (o + 4 <= e) {
                const int step = rd16(d, o), v = rd16(d, o + 2);
                if (v <= 0 || v > 500 || step > 4000) break;
                const int us = 60000000 / bpmForTempoValue(v);
                if (us != prevUs) { tempi.push_back({ tick, us }); prevUs = us; }
                tick += step;
                o += 4;
            }
        }
    }

    // A song-wide transpose sits in the settings area, signed, right after the
    // master volume and the default tempo.
    int transpose = 0;
    {
        const int S = 0x46 + 4 * (L.parts + 1);
        if (S + 11 <= n) transpose = int(int16_t(rd16(d, S + 9)));
    }

    // ---- parts
    std::vector<Track> tracks;
    for (int k = 1; k <= L.parts; ++k) {
        const int s = L.blockStart[size_t(k)];
        const int blockEnd = std::min(n, s + L.blockSize[size_t(k)]);
        if (s + 158 > n) break;

        Track t;
        t.channel = k - 1;
        {
            int len = 0;
            while (len < 12 && d[s + 81 + len] != 0x00) ++len;
            t.name = QByteArray(reinterpret_cast<const char*>(d + s + 81), len);
        }
        const int module = d[s + 94];
        const int used   = rd16(d, s + 112);
        const int ctl    = rd16(d, s + 114);
        const int a1     = s + 158;
        const int a2     = a1 + used;

        // Drum keys are slots, not pitches, so the musical transpose skips the
        // rhythm part - module 2 on an MT-32, 5 on a GS module.
        const int tr = (module == 2 || module == 5) ? 0 : transpose;

        // Notes. Eight bytes: key, gate, step, a signed nudge that moves the
        // note without moving the clock, velocity, two engraving bytes, flags.
        // Bit 7 of the flags means the record continues the note before it -
        // one sound written as two heads across a bar line.
        int tick = 0;
        int lastKey = -1, lastOffIdx = -1, lastOffTick = -1;
        for (int i = 0; i * 8 + 8 <= used && a1 + i * 8 + 8 <= blockEnd; ++i) {
            const unsigned char* r = d + a1 + i * 8;
            if (r[0] == 0xFC) break;                    // end of the part
            const int step  = r[2];
            const int shift = int(int8_t(r[3]));
            if (r[1] != 0) {
                const int at  = tick + shift;
                const int key = std::max(0, std::min(127, int(r[0]) + tr));
                if ((r[7] & 0x80) && key == lastKey && lastOffIdx >= 0 && lastOffTick == at) {
                    t.events[size_t(lastOffIdx)].tick = at + r[1];
                    lastOffTick = at + r[1];
                } else {
                    Event on { at, 2, { uint8_t(0x90 | t.channel), uint8_t(key), r[4] }, 3 };
                    Event off{ at + r[1], 1, { uint8_t(0x90 | t.channel), uint8_t(key), 0 }, 3 };
                    t.events.push_back(on);
                    t.events.push_back(off);
                    lastKey = key; lastOffIdx = int(t.events.size()) - 1; lastOffTick = off.tick;
                }
            }
            tick += step;
        }

        // Controllers, on their own clock of the same length: status, step,
        // and the two data bytes. Status 0x00 is a wait and nothing else.
        tick = 0;
        for (int i = 0; i * 4 + 4 <= ctl && a2 + i * 4 + 4 <= blockEnd; ++i) {
            const unsigned char* r = d + a2 + i * 4;
            if (r[0] == 0xFC) break;
            const int st = r[0] & 0xF0;
            if (st == 0xB0 || st == 0xE0)
                t.events.push_back({ tick, 0, { uint8_t(st | t.channel),
                                                uint8_t(r[2] & 0x7F), uint8_t(r[3] & 0x7F) }, 3 });
            else if (st == 0xC0)
                t.events.push_back({ tick, 0, { uint8_t(0xC0 | t.channel),
                                                uint8_t(r[2] & 0x7F), 0 }, 2 });
            tick += r[1];
        }
        tracks.push_back(std::move(t));
    }
    if (tracks.empty()) return QByteArray();

    // ---- assemble. Ballade counts one bar in before the song, and every
    // stream is written relative to the end of it.
    const int lead = barLen;
    std::vector<uint8_t> out;
    const uint16_t nchunks = uint16_t(tracks.size() + 1);
    out.insert(out.end(), { 'M','T','h','d' });
    writeBE32(out, 6);
    out.push_back(0); out.push_back(1);
    out.push_back(uint8_t(nchunks >> 8)); out.push_back(uint8_t(nchunks & 0xFF));
    out.push_back(uint8_t(kTicksPerQuarter >> 8)); out.push_back(uint8_t(kTicksPerQuarter & 0xFF));

    auto emitChunk = [&out](const std::vector<uint8_t>& body) {
        out.insert(out.end(), { 'M','T','r','k' });
        writeBE32(out, uint32_t(body.size() + 4));
        out.insert(out.end(), body.begin(), body.end());
        out.insert(out.end(), { 0x00, 0xFF, 0x2F, 0x00 });
    };

    {   // conductor
        std::vector<uint8_t> body;
        int denPow = 0;
        for (int v = sigDen; v > 1; v >>= 1) ++denPow;
        writeVarLen(body, 0);
        body.insert(body.end(), { 0xFF, 0x58, 0x04, uint8_t(sigNum), uint8_t(denPow), 24, 8 });
        int prev = 0;
        const int firstUs = tempi.empty() ? 500000 : tempi.front().us;
        writeVarLen(body, 0);
        body.insert(body.end(), { 0xFF, 0x51, 0x03,
                                  uint8_t(firstUs >> 16), uint8_t(firstUs >> 8), uint8_t(firstUs) });
        for (const TempoPoint& tp : tempi) {
            const int at = tp.tick + lead;
            if (at <= prev && !body.empty() && tp.tick == 0) continue;
            writeVarLen(body, uint32_t(at - prev));
            body.insert(body.end(), { 0xFF, 0x51, 0x03,
                                      uint8_t(tp.us >> 16), uint8_t(tp.us >> 8), uint8_t(tp.us) });
            prev = at;
        }
        emitChunk(body);
    }

    for (Track& t : tracks) {
        std::stable_sort(t.events.begin(), t.events.end(),
                         [](const Event& a, const Event& b) {
                             if (a.tick != b.tick) return a.tick < b.tick;
                             return a.order < b.order;
                         });
        std::vector<uint8_t> body;
        writeVarLen(body, 0);
        body.push_back(0xFF); body.push_back(0x03);
        writeVarLen(body, uint32_t(t.name.size()));
        body.insert(body.end(), t.name.begin(), t.name.end());
        // Ballade's bends assume the GM default of two semitones; say so, so a
        // module left on some other range by the previous song still plays.
        writeVarLen(body, 0);
        body.insert(body.end(), { uint8_t(0xB0 | t.channel), 101, 0 });
        writeVarLen(body, 0);
        body.insert(body.end(), { uint8_t(0xB0 | t.channel), 100, 0 });
        writeVarLen(body, 0);
        body.insert(body.end(), { uint8_t(0xB0 | t.channel), 6, 2 });

        int prev = 0;
        for (const Event& e : t.events) {
            const int at = std::max(0, e.tick + lead);
            writeVarLen(body, uint32_t(std::max(0, at - prev)));
            body.insert(body.end(), e.bytes, e.bytes + e.len);
            prev = at;
        }
        emitChunk(body);
    }

    return QByteArray(reinterpret_cast<const char*>(out.data()), int(out.size()));
}

QByteArray toMidi(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return toMidi(f.readAll());
}

} // namespace sngmidi
