// Standard MIDI File reading and writing.
#pragma once

#include <vector>

#include "byteio.h"
#include "song.h"

namespace jmpconv {

struct SmfTrackEvents {
    std::vector<NoteEvent> notes;
    std::vector<ControlEvent> controls;
    std::vector<TempoEvent> tempo;
};

// Total byte length of the SMF starting at the front of `data`, or 0 if there
// is no well-formed one. Containers put other blocks straight after the music,
// so the length has to come from the chunk walk, not from the file size.
size_t smfLength(const Bytes& data);

int smfDivision(const Bytes& data);

// One entry per MTrk, conductor track included.
std::vector<SmfTrackEvents> smfParse(const Bytes& data);

// Build a format-1 SMF. `conductor` becomes track 0.
Bytes smfBuild(int division, const std::vector<TempoEvent>& conductor,
               const std::vector<Track>& tracks);

}  // namespace jmpconv
