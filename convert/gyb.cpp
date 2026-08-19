#include "gyb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "oksori.h"   // GYB and OKA share the 38-byte instrument record
#include "oplbank.h"
#include "smf.h"

namespace jmpconv {

namespace {

struct ControlBlock {
    std::vector<std::pair<uint32_t, int>> events;
};

// Reads one "u16 count, then count 4-byte records" block. Records at or past
// `endTick` are discarded, which is what the reference output does.
ControlBlock readControls(const Bytes& d, size_t& pos, uint32_t endTick) {
    ControlBlock blk;
    if (pos + 2 > d.size()) return blk;
    uint16_t n = rdLE16(&d[pos]);
    pos += 2;
    for (uint16_t i = 0; i < n && pos + 4 <= d.size(); ++i, pos += 4) {
        uint32_t tick = rdLE16(&d[pos]);
        if (tick < endTick) blk.events.emplace_back(tick, d[pos + 2]);
    }
    return blk;
}

}  // namespace

bool gybRead(const Bytes& d, GybFile& out) {
    out = GybFile();
    if (d.size() < 0x50) return false;
    if (d[0] != 0x03 && d[0] != 0x04) return false;

    out.tickBeat = rdLE16(&d[0x28]);
    out.timeSigNum = d[0x2A];
    out.timeSigDen = d[0x2B];
    out.headerBpm = rdLE16(&d[0x32]);
    // 0x34 holds the base tempo as BPM x 100, exact on all 27 reference pairs.
    // The field at 0x32 that jmp labels BPM is something else -- it reads 100
    // on songs that play at 60 and at 200.
    uint16_t base = rdLE16(&d[0x34]);
    if (base) out.baseBpm = base / 100.0;
    int instCount = rdLE16(&d[0x3C]);

    size_t pos = (d[0] == 0x03) ? 0x40 : 0x4F;
    uint16_t globals = rdLE16(&d[pos]);
    pos += 2;
    for (uint16_t i = 0; i < globals && pos + 4 <= d.size(); ++i, pos += 4) {
        out.globalTicks.push_back(rdLE16(&d[pos]));
        out.globalValues.push_back(rdLE16(&d[pos + 2]));
    }

    for (int ch = 0; ch < 11; ++ch) {
        if (pos + 3 > d.size() || d[pos] != ch) break;
        ++pos;
        uint32_t endTick = rdLE16(&d[pos]);
        pos += 2;

        struct Raw { uint32_t tick; uint8_t cmd; };
        std::vector<Raw> raw;
        uint32_t tick = 0;
        while (tick < endTick && pos + 2 <= d.size()) {
            uint8_t cmd = d[pos], dur = d[pos + 1];
            pos += 2;
            if (cmd) raw.push_back({tick, cmd});
            tick += dur;
        }

        ControlBlock prog = readControls(d, pos, endTick);
        ControlBlock vol = readControls(d, pos, endTick);
        ControlBlock pitch = readControls(d, pos, endTick);

        Track tr;
        tr.channel = ch;
        for (const auto& e : prog.events)
            tr.controls.push_back({e.first * kGybTickScale,
                                   ControlEvent::Program, e.second});
        for (const auto& e : vol.events)
            tr.controls.push_back({e.first * kGybTickScale,
                                   ControlEvent::Volume,
                                   gybVolumeToMidi(e.second)});
        for (const auto& e : pitch.events)
            tr.controls.push_back({e.first * kGybTickScale,
                                   ControlEvent::PitchBend,
                                   gybPitchToBend(e.second)});

        size_t vi = 0;
        int velocity = vol.events.empty() ? 127
                                          : gybVolumeToMidi(vol.events[0].second);
        for (const Raw& r : raw) {
            while (vi < vol.events.size() && vol.events[vi].first <= r.tick) {
                velocity = gybVolumeToMidi(vol.events[vi].second);
                ++vi;
            }
            NoteEvent n;
            n.tick = r.tick * kGybTickScale;
            n.on = r.cmd < kGybNoteOffBias;
            n.pitch = n.on ? r.cmd : uint8_t(r.cmd - kGybNoteOffBias);
            n.velocity = n.on ? uint8_t(velocity) : 0;
            tr.notes.push_back(n);
        }
        out.tracks.push_back(std::move(tr));
    }

    for (int i = 0; i < instCount && pos + kInstRecord <= d.size(); ++i) {
        Instrument ins;
        size_t len = 0;
        while (len < 9 && d[pos + len] != 0) ++len;
        ins.name.assign(reinterpret_cast<const char*>(&d[pos]), len);
        ins.flag = d[pos + 9];
        std::memcpy(ins.params.data(), &d[pos + 10], 28);
        out.instruments.push_back(std::move(ins));
        pos += kInstRecord;
    }
    if (pos < d.size()) out.lyricStream.assign(d.begin() + pos, d.end());

    out.valid = !out.tracks.empty();
    return out.valid;
}

bool gybReadFile(const std::string& path, GybFile& out) {
    Bytes raw;
    return readFile(path, raw) && gybRead(raw, out);
}

Song gybToSong(const GybFile& g, double baseBpmOverride) {
    Song s;
    s.division = (g.tickBeat & 0xFF) * kGybTickScale;
    s.instruments = g.instruments;

    const double base = baseBpmOverride > 0.0 ? baseBpmOverride : g.baseBpm;
    for (size_t i = 0; i < g.globalValues.size(); ++i) {
        double bpm = g.globalValues[i] * base / 100.0;
        if (bpm <= 0) continue;
        s.tempo.push_back({uint32_t(g.globalTicks[i] * kGybTickScale),
                           uint32_t(std::llround(60000000.0 / bpm))});
    }
    for (const Track& t : g.tracks)
        if (!t.empty()) s.tracks.push_back(t);
    return s;
}

Bytes gybWrite(const GybFile& g) {
    Bytes out(0x42, 0);
    out[0] = 0x03; // Header magic
    wrLE16(&out[0x28], uint16_t(g.tickBeat != 0 ? g.tickBeat : 4));
    wrLE16(&out[0x34], uint16_t(g.baseBpm > 0 ? (g.baseBpm * 100.0 + 0.5) : 12000));
    wrLE16(&out[0x3C], 11); // 11 instrument slots

    // Global tempo map at 0x40
    wrLE16(&out[0x40], uint16_t(g.globalValues.empty() ? 1 : g.globalValues.size()));
    size_t pos = 0x42;
    if (g.globalValues.empty()) {
        out.resize(pos + 4, 0);
        wrLE16(&out[pos], 0);
        wrLE16(&out[pos + 2], 100);
        pos += 4;
    } else {
        out.resize(pos + g.globalValues.size() * 4);
        for (size_t i = 0; i < g.globalValues.size(); ++i) {
            wrLE16(&out[pos + i * 4], uint16_t(g.globalTicks[i]));
            wrLE16(&out[pos + i * 4 + 2], uint16_t(g.globalValues[i]));
        }
        pos += g.globalValues.size() * 4;
    }

    // Write 11 channel blocks with accurate note timing and rests
    for (int ch = 0; ch < 11; ++ch) {
        out.push_back(uint8_t(ch));

        const Track* trk = nullptr;
        for (const auto& t : g.tracks) {
            if (t.channel == ch) { trk = &t; break; }
        }

        Bytes noteStream;
        uint32_t currentGybTick = 0;

        if (trk && !trk->notes.empty()) {
            // Pair Note-On with Note-Off to find exact durations
            struct NotePair {
                uint32_t startTick;
                uint32_t dur;
                uint8_t pitch;
            };
            std::vector<NotePair> pairs;

            for (size_t i = 0; i < trk->notes.size(); ++i) {
                const auto& n = trk->notes[i];
                if (n.on) {
                    uint32_t onTick = n.tick / kGybTickScale;
                    uint32_t dur = 4; // Default fallback
                    // Find matching off or next on
                    for (size_t j = i + 1; j < trk->notes.size(); ++j) {
                        if (trk->notes[j].pitch == n.pitch && !trk->notes[j].on) {
                            uint32_t offTick = trk->notes[j].tick / kGybTickScale;
                            if (offTick > onTick) dur = offTick - onTick;
                            break;
                        }
                    }
                    pairs.push_back({onTick, dur, uint8_t(std::clamp<int>(n.pitch, 1, 120))});
                }
            }

            for (const auto& p : pairs) {
                // Insert rest if there is a gap before this note
                if (p.startTick > currentGybTick) {
                    uint32_t gap = p.startTick - currentGybTick;
                    while (gap > 0) {
                        uint8_t chunk = uint8_t(std::min<uint32_t>(gap, 255));
                        noteStream.push_back(0x00); // Rest cmd
                        noteStream.push_back(chunk);
                        gap -= chunk;
                    }
                    currentGybTick = p.startTick;
                }

                // Insert the note
                uint32_t noteDur = p.dur;
                while (noteDur > 0) {
                    uint8_t chunk = uint8_t(std::min<uint32_t>(noteDur, 255));
                    noteStream.push_back(p.pitch);
                    noteStream.push_back(chunk);
                    noteDur -= chunk;
                }
                currentGybTick += p.dur;
            }
        }

        uint16_t endTick = uint16_t(currentGybTick > 0 ? currentGybTick : 100);
        size_t endOff = out.size();
        out.resize(endOff + 2);
        wrLE16(&out[endOff], endTick);

        // Append note stream
        out.insert(out.end(), noteStream.begin(), noteStream.end());

        // 3 control blocks: program, volume, pitch
        // Write default initial volume and program so the voice actually sounds!
        // 1. Program Change (1 event at tick 0)
        size_t progOff = out.size();
        out.resize(progOff + 6, 0);
        wrLE16(&out[progOff], 1);      // 1 prog event
        wrLE16(&out[progOff + 2], 0);  // tick 0
        out[progOff + 4] = uint8_t(ch); // prog = ch
        out[progOff + 5] = 0;

        // 2. Volume (1 event at tick 0, volume 90%)
        size_t volOff = out.size();
        out.resize(volOff + 6, 0);
        wrLE16(&out[volOff], 1);      // 1 vol event
        wrLE16(&out[volOff + 2], 0);  // tick 0
        out[volOff + 4] = 90;         // vol 90
        out[volOff + 5] = 0;

        // 3. Pitch Bend (1 event at tick 0, center = 10)
        size_t pitOff = out.size();
        out.resize(pitOff + 6, 0);
        wrLE16(&out[pitOff], 1);      // 1 pitch event
        wrLE16(&out[pitOff + 2], 0);  // tick 0
        out[pitOff + 4] = 10;         // center bend = 10
        out[pitOff + 5] = 0;
    }

    // 11 instrument records (418 bytes) with enriched OPL parameters
    Bytes instTab(418, 0);
    auto insts = g.instruments;
    enrichInstruments(insts);

    for (size_t i = 0; i < insts.size() && i < 11; ++i) {
        size_t off = i * 38;
        const auto& inst = insts[i];
        std::memcpy(&instTab[off], inst.name.data(), std::min<size_t>(inst.name.size(), 9));
        instTab[off + 9] = inst.flag;
        std::memcpy(&instTab[off + 10], inst.params.data(), 28);
    }
    out.insert(out.end(), instTab.begin(), instTab.end());

    if (!g.lyricStream.empty()) {
        out.insert(out.end(), g.lyricStream.begin(), g.lyricStream.end());
    }

    return out;
}

Bytes gybToOka(const GybFile& g, double baseBpmOverride) {
    Song s = gybToSong(g, baseBpmOverride);
    Bytes smf = smfBuild(s.division, s.tempo, s.tracks);

    OkaFile o;
    o.midi = smf;
    o.instruments = g.instruments;
    enrichInstruments(o.instruments);
    o.meta.resize(0x187, 0);

    if (!g.lyricStream.empty()) {
        o.lyricBlock.assign(g.lyricStream.begin(), g.lyricStream.end());
    }

    return okaWrite(o);
}

}  // namespace jmpconv
