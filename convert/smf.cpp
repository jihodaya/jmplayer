#include "smf.h"

#include <algorithm>
#include <fstream>

namespace jmpconv {

bool readFile(const std::string& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    f.seekg(0);
    out.resize(size_t(n));
    return bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

bool writeFile(const std::string& path, const Bytes& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

size_t smfLength(const Bytes& d) {
    if (d.size() < 14 || d[0] != 'M' || d[1] != 'T' || d[2] != 'h' || d[3] != 'd')
        return 0;
    size_t ntrk = rdBE16(&d[10]);
    size_t off = 8 + rdBE32(&d[4]);
    for (size_t i = 0; i < ntrk; ++i) {
        if (off + 8 > d.size() || d[off] != 'M' || d[off + 1] != 'T' ||
            d[off + 2] != 'r' || d[off + 3] != 'k')
            return 0;
        off += 8 + rdBE32(&d[off + 4]);
        if (off > d.size()) return 0;
    }
    return off;
}

int smfDivision(const Bytes& d) {
    return d.size() >= 14 ? rdBE16(&d[12]) : 0;
}

std::vector<SmfTrackEvents> smfParse(const Bytes& d) {
    std::vector<SmfTrackEvents> out;
    if (!smfLength(d)) return out;

    size_t ntrk = rdBE16(&d[10]);
    size_t off = 8 + rdBE32(&d[4]);
    for (size_t i = 0; i < ntrk; ++i) {
        size_t end = off + 8 + rdBE32(&d[off + 4]);
        size_t p = off + 8;
        uint32_t tick = 0;
        uint8_t running = 0;
        SmfTrackEvents ev;

        while (p < end) {
            tick += readVarLen(d.data(), p);
            uint8_t b = d[p];
            uint8_t status = (b < 0x80) ? running : b;
            if (b >= 0x80) ++p;

            if (status == 0xFF) {
                uint8_t meta = d[p++];
                uint32_t len = readVarLen(d.data(), p);
                if (meta == 0x51 && len == 3) {
                    uint32_t us = (uint32_t(d[p]) << 16) | (uint32_t(d[p + 1]) << 8) | d[p + 2];
                    ev.tempo.push_back({tick, us});
                }
                p += len;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                p += readVarLen(d.data(), p);
                continue;
            }

            running = status;
            uint8_t hi = status & 0xF0;
            if (hi == 0x90) {
                // Classified by status, not by velocity. A GYB channel really
                // can hold volume-0 notes -- 335 of them across the corpus --
                // and folding those into note offs loses them. We write real
                // offs as 0x80 so the two stay distinguishable.
                ev.notes.push_back({tick, d[p], d[p + 1], true});
                p += 2;
            } else if (hi == 0x80) {
                ev.notes.push_back({tick, d[p], d[p + 1], false});
                p += 2;
            } else if (hi == 0xC0) {
                ev.controls.push_back({tick, ControlEvent::Program, d[p]});
                p += 1;
            } else if (hi == 0xD0) {
                p += 1;
            } else if (hi == 0xB0) {
                if (d[p] == 7)
                    ev.controls.push_back({tick, ControlEvent::Volume, d[p + 1]});
                p += 2;
            } else if (hi == 0xE0) {
                ev.controls.push_back({tick, ControlEvent::PitchBend,
                                       d[p] | (d[p + 1] << 7)});
                p += 2;
            } else {
                p += 2;
            }
        }
        out.push_back(std::move(ev));
        off = end;
    }
    return out;
}

namespace {

struct RawEvent {
    uint32_t tick;
    int order;          // keeps controls ahead of notes at the same tick
    Bytes payload;
};

void appendTrack(Bytes& out, Bytes body) {
    body.push_back(0x00);
    body.push_back(0xFF);
    body.push_back(0x2F);
    body.push_back(0x00);
    out.push_back('M'); out.push_back('T'); out.push_back('r'); out.push_back('k');
    pushBE32(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

Bytes smfBuild(int division, const std::vector<TempoEvent>& conductor,
               const std::vector<Track>& tracks) {
    Bytes out;
    out.push_back('M'); out.push_back('T'); out.push_back('h'); out.push_back('d');
    pushBE32(out, 6);
    pushBE16(out, 1);
    pushBE16(out, uint16_t(tracks.size() + 1));
    pushBE16(out, uint16_t(division));

    {   // conductor
        Bytes body;
        uint32_t last = 0;
        for (const TempoEvent& t : conductor) {
            pushVarLen(body, t.tick - last);
            last = t.tick;
            body.push_back(0xFF); body.push_back(0x51); body.push_back(0x03);
            body.push_back(uint8_t(t.usecPerQuarter >> 16));
            body.push_back(uint8_t(t.usecPerQuarter >> 8));
            body.push_back(uint8_t(t.usecPerQuarter));
        }
        appendTrack(out, std::move(body));
    }

    for (const Track& tr : tracks) {
        std::vector<RawEvent> events;
        uint8_t ch = uint8_t(tr.channel & 0x0F);

        for (const ControlEvent& c : tr.controls) {
            Bytes e;
            if (c.kind == ControlEvent::Program) {
                e = {uint8_t(0xC0 | ch), uint8_t(c.value)};
            } else if (c.kind == ControlEvent::Volume) {
                e = {uint8_t(0xB0 | ch), 0x07, uint8_t(c.value)};
            } else {
                e = {uint8_t(0xE0 | ch), uint8_t(c.value & 0x7F),
                     uint8_t((c.value >> 7) & 0x7F)};
            }
            events.push_back({c.tick, 0, std::move(e)});
        }
        for (const NoteEvent& n : tr.notes) {
            events.push_back({n.tick, 1,
                              {uint8_t((n.on ? 0x90 : 0x80) | ch), n.pitch,
                               uint8_t(n.on ? n.velocity : 0)}});
        }

        std::stable_sort(events.begin(), events.end(),
                         [](const RawEvent& a, const RawEvent& b) {
                             return a.tick != b.tick ? a.tick < b.tick
                                                     : a.order < b.order;
                         });

        Bytes body;
        uint32_t last = 0;
        for (const RawEvent& e : events) {
            pushVarLen(body, e.tick - last);
            last = e.tick;
            body.insert(body.end(), e.payload.begin(), e.payload.end());
        }
        appendTrack(out, std::move(body));
    }
    return out;
}

}  // namespace jmpconv
