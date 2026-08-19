// The intermediate representation every reader produces and every writer
// consumes. One hub, so any source format can reach any target without
// chaining conversions and compounding their losses.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "byteio.h"

namespace jmpconv {

// An OPL patch as GYB and OKA store it: a 9-byte name, a flag whose bit 0 says
// whether the parameters are real, then 28 parameter bytes. Files converted
// from .ROL carry the name only and leave the parameters zero, which is why
// they play silent until a bank resolves them.
struct Instrument {
    std::string name;
    uint8_t flag = 0;
    std::array<uint8_t, 28> params{};

    bool hasParams() const { return (flag & 1) != 0; }
};

struct NoteEvent {
    uint32_t tick = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    bool on = true;
};

struct ControlEvent {
    enum Kind { Program, Volume, PitchBend };
    uint32_t tick = 0;
    Kind kind = Program;
    int value = 0;
};

struct TempoEvent {
    uint32_t tick = 0;
    uint32_t usecPerQuarter = 500000;
};

struct Track {
    int channel = 0;
    std::vector<NoteEvent> notes;
    std::vector<ControlEvent> controls;

    bool empty() const { return notes.empty() && controls.empty(); }
};

struct Song {
    int division = 120;               // SMF ticks per quarter note
    std::vector<TempoEvent> tempo;
    std::vector<Track> tracks;
    std::vector<Instrument> instruments;

    // Carried through untouched for now: the Oksori text area and the lyric /
    // text / spare payload blocks. Interpreting these is what will let lyrics
    // move between formats.
    Bytes meta;
    Bytes lyricBlock;
    Bytes textBlock;
    Bytes extraBlock;
    Bytes tail;
    uint16_t sampleRate = 0;          // OKW recordings only
};

}  // namespace jmpconv
