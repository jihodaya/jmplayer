// OPL patch names to General MIDI / GS.
//
// This is the one genuinely lossy step in the whole converter. A program
// change in an OKA or GYB is an index into the song's own OPL instrument
// table, not a GM program, so playing those numbers on a GM synth gives
// nonsense -- slot 11 might be a bass drum and land on Vibraphone.
//
// The names come from the STANDARD.BNK vocabulary the DOS tools shared, so the
// mapping is by name. Percussion goes to GM channel 10 with a real drum note,
// and where GS has a closer voice than plain GM we select its variation bank.
// Everything here is only a starting point: the caller can edit any assignment
// before the conversion runs.
#pragma once

#include <string>
#include <vector>

#include "song.h"

namespace jmpconv {

constexpr int kGmDrumChannel = 9;      // zero-based; GM channel 10
constexpr int kGmMelodicRescue = 11;   // where a melodic part on 9 is moved

// One editable row: what a given OPL slot on a given track will become.
struct PatchAssignment {
    int track = 0;              // MTrk index in the source
    int channel = 0;            // source MIDI channel
    int slot = -1;              // OPL instrument slot the program change names
    std::string name;           // the OPL patch name, for display
    long notes = 0;             // how much of the song this actually plays

    bool drum = false;          // route to channel 10 as percussion
    int bankMsb = 0;            // GS variation bank (CC0); 0 is plain GM
    int program = 0;            // GM program
    int drumNote = 38;          // GM percussion note when drum
    bool autoMatched = false;   // false means the name was not recognised
};

// The default assignment for one OPL patch name.
PatchAssignment gmForInstrument(const std::string& oplName);

// Work out a full plan for a song: one row per (track, slot) actually used.
std::vector<PatchAssignment> planPatches(const Bytes& midi,
                                         const std::vector<Instrument>& slots);

struct GmStats {
    int drumTracks = 0;
    int notesEnded = 0;      // note offs we had to add
    int channelsMoved = 0;   // melodic tracks taken off the drum channel
};

// Rewrites an SMF so it plays correctly on a GM/GS device. Three things are
// wrong with the source as it stands:
//
//  1. Program changes index the song's own OPL table, not GM.
//  2. Percussion sits on ordinary channels, so it plays as pitched notes.
//  3. There are almost no note offs -- 87 against 25559 note ons across eight
//     reference songs. An OPL channel is monophonic, so a new note cuts the
//     previous one and the DOS format never needed to say so. A polyphonic GM
//     synth instead holds every note forever, which is what makes an
//     untreated conversion sound like mush.
//
// So we apply `plan`, move percussion to channel 10, move any melodic track
// off channel 10, and end each sounding note when its channel starts the next
// one. Meta events, lyrics, SysEx and timing are copied through untouched.
// With `gsReset` a GS reset is emitted up front so variation banks take.
Bytes smfToGeneralMidi(const Bytes& midi,
                       const std::vector<PatchAssignment>& plan,
                       bool gsReset = true, GmStats* stats = nullptr);

// Names for the GUI.
const char* gmProgramName(int program);
const char* gmDrumName(int note);
const std::vector<int>& gmDrumNotes();

struct GsBankOption {
    int bankMsb = 0;
    const char* name = nullptr;
};
const std::vector<GsBankOption>& gsBankOptions();

}  // namespace jmpconv
