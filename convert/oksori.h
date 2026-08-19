// The "Oksori Music File" container: .OKA, .OKM and .OKW.
//
// Header layout, verified byte for byte against 86 reference files:
//
//   0x000  "Oksori Music File", then 1A 0A 10 at 0x1A
//   0x027  title + lyric-preview text area (Johab); melody channel at 0x15A
//   0x1AE  u32 lyric block size
//   0x1B2  u16 text block size
//   0x1B4  u16 spare block size
//   0x1CA  u16 total payload size (all five blocks)
//   0x1CE  u32 MIDI size
//   0x1D2  u16 instrument block size
//   0x1F6  u16 recording sample rate (OKW only)
//   0x1F8  u8  instrument record count
//   0x210  plaintext copy of the first 256 bytes of the decrypted SMF
//   0x310  payload: MIDI, lyric, text, spare, instruments -- all XOR 0xA8
#pragma once

#include <string>

#include "song.h"

namespace jmpconv {

struct OkaFile {
    Bytes meta;                       // 0x27..0x1AE, carried verbatim
    Bytes midi;                       // decrypted SMF
    Bytes lyricBlock;
    Bytes textBlock;
    Bytes extraBlock;
    Bytes tail;                       // OKW keeps its recording here
    std::vector<Instrument> instruments;
    uint16_t sampleRate = 0;
    uint8_t melodyChannel = 0;

    bool valid = false;
};

bool okaRead(const Bytes& raw, OkaFile& out);
bool okaReadFile(const std::string& path, OkaFile& out);

// Rebuilds the file. Every size field, the instrument count and the plaintext
// music mirror are recomputed, so a byte-identical result means the layout is
// understood rather than copied.
Bytes okaWrite(const OkaFile& f);
Bytes okaToGyb(const OkaFile& oka);

constexpr uint8_t kOkaXor = 0xA8;
constexpr int kInstRecord = 38;
constexpr int kInstName = 9;

}  // namespace jmpconv
