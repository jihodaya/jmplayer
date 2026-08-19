#include "oksori.h"

#include <cstring>

#include "gyb.h"
#include "smf.h"

namespace jmpconv {

namespace {

constexpr size_t kPayload = 0x310;
constexpr size_t kMirror = 0x210;
constexpr size_t kMirrorLen = 0x100;

Bytes decode(const Bytes& raw, size_t at, size_t n) {
    Bytes out;
    if (at >= raw.size()) return out;
    n = std::min(n, raw.size() - at);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = raw[at + i] ^ kOkaXor;
    return out;
}

std::vector<Instrument> parseInstruments(const Bytes& tab) {
    std::vector<Instrument> out;
    for (size_t i = 0; i + kInstRecord <= tab.size(); i += kInstRecord) {
        Instrument ins;
        size_t len = 0;
        while (len < kInstName && tab[i + len] != 0) ++len;
        ins.name.assign(reinterpret_cast<const char*>(&tab[i]), len);
        ins.flag = tab[i + kInstName];
        std::memcpy(ins.params.data(), &tab[i + kInstName + 1], 28);
        out.push_back(std::move(ins));
    }
    return out;
}

}  // namespace

bool okaRead(const Bytes& raw, OkaFile& out) {
    out = OkaFile();
    if (raw.size() < kPayload) return false;
    if (std::memcmp(raw.data(), "Oksori Music File", 17) != 0) return false;

    out.meta.assign(raw.begin() + 0x27, raw.begin() + 0x1AE);
    out.melodyChannel = raw[0x15A];
    out.sampleRate = rdLE16(&raw[0x1F6]);

    uint32_t lyricSz = rdLE32(&raw[0x1AE]);
    uint16_t textSz = rdLE16(&raw[0x1B2]);
    uint16_t extraSz = rdLE16(&raw[0x1B4]);
    uint32_t midiSz = rdLE32(&raw[0x1CE]);
    uint16_t instSz = rdLE16(&raw[0x1D2]);

    size_t at = kPayload;
    out.midi = decode(raw, at, midiSz);            at += midiSz;
    out.lyricBlock = decode(raw, at, lyricSz);     at += lyricSz;
    out.textBlock = decode(raw, at, textSz);       at += textSz;
    out.extraBlock = decode(raw, at, extraSz);     at += extraSz;

    if (at + instSz > raw.size()) return false;
    out.instruments = parseInstruments(decode(raw, at, instSz));
    at += instSz;
    out.tail.assign(raw.begin() + at, raw.end());

    out.valid = smfLength(out.midi) != 0;
    return out.valid;
}

bool okaReadFile(const std::string& path, OkaFile& out) {
    Bytes raw;
    return readFile(path, raw) && okaRead(raw, out);
}

Bytes okaWrite(const OkaFile& f) {
    Bytes head(kPayload, 0);
    std::memcpy(head.data(), "Oksori Music File", 17);
    head[0x1A] = 0x1A;
    head[0x1B] = 0x0A;
    head[0x1C] = 0x10;
    if (f.meta.size() == 0x1AE - 0x27)
        std::memcpy(&head[0x27], f.meta.data(), f.meta.size());

    Bytes inst;
    inst.reserve(f.instruments.size() * kInstRecord);
    for (const Instrument& i : f.instruments) {
        size_t start = inst.size();
        inst.resize(start + kInstRecord, 0);
        std::memcpy(&inst[start], i.name.data(),
                    std::min<size_t>(i.name.size(), kInstName));
        inst[start + kInstName] = i.flag;
        std::memcpy(&inst[start + kInstName + 1], i.params.data(), 28);
    }

    uint32_t payload = uint32_t(f.midi.size() + f.lyricBlock.size() +
                                f.textBlock.size() + f.extraBlock.size() +
                                inst.size());

    wrLE32(&head[0x1AE], uint32_t(f.lyricBlock.size()));
    wrLE16(&head[0x1B2], uint16_t(f.textBlock.size()));
    wrLE16(&head[0x1B4], uint16_t(f.extraBlock.size()));
    wrLE16(&head[0x1CA], uint16_t(payload));
    wrLE32(&head[0x1CE], uint32_t(f.midi.size()));
    wrLE16(&head[0x1D2], uint16_t(inst.size()));
    head[0x15A] = f.melodyChannel != 0 ? f.melodyChannel : 0x01;
    wrLE16(&head[0x1F6], f.sampleRate);
    head[0x1F2] = 0x01;
    head[0x1F8] = uint8_t(inst.size() / kInstRecord);

    size_t mirror = std::min(kMirrorLen, f.midi.size());
    std::memcpy(&head[kMirror], f.midi.data(), mirror);

    Bytes body;
    body.reserve(payload);
    for (const Bytes* b : {&f.midi, &f.lyricBlock, &f.textBlock, &f.extraBlock})
        body.insert(body.end(), b->begin(), b->end());
    body.insert(body.end(), inst.begin(), inst.end());
    for (uint8_t& c : body) c ^= kOkaXor;

    Bytes out = std::move(head);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), f.tail.begin(), f.tail.end());
    return out;
}

Bytes okaToGyb(const OkaFile& oka) {
    GybFile g;
    auto parsed = smfParse(oka.midi);
    int ch = 0;
    for (const auto& pe : parsed) {
        if (!pe.notes.empty() || !pe.controls.empty()) {
            Track t;
            t.channel = ch++;
            t.notes = pe.notes;
            t.controls = pe.controls;
            g.tracks.push_back(t);
        }
    }
    g.instruments = oka.instruments;
    g.baseBpm = 120.0;
    if (!oka.lyricBlock.empty()) {
        g.lyricStream.assign(oka.lyricBlock.begin(), oka.lyricBlock.end());
    }
    return gybWrite(g);
}

}  // namespace jmpconv
