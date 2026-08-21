// The MT-32 tone list GAYOBANG and NORE45 assign from, and how those tones
// reach General MIDI.
//
// A .GYB / .OKA instrument record carries a flag byte at offset 9. Bit 0 says
// whether the 28 OPL parameters that follow are real - that is the only part
// this player used until now. The other seven bits are the **MIDI tone the
// song was assigned when someone played it through a sound module**:
//
//     flag = (tone << 1) | paramsValid          tone is 0-based, displayed +1
//
// GAYOBANG's own code says so plainly. Its instrument dialog draws the name
// with `(flag[slot * 0x26] >> 1) * 0xf + toneTable + 3` - the flag shifted past
// bit 0 indexes a 15-byte-per-entry name table, +3 skipping the "001" prefix
// each entry starts with. The table is at file offset 0x3924b in GAYOBANG.EXE
// and is the Roland MT-32's 128 preset tones, verbatim: `Fantasy`,
// `Harmo Pan`, `Doctor Solo`, `Schooldaze`, `Bellsinger`, `Oboe 2001` and
// `Jungle Tune` exist on no other module.
//
// Decoded across the library the assignments read `elviolin`->violin1,
// `accordn`->Accordion, `guitar0`->Guitar1, `snare1b`->Deep Snare,
// `cymbal1`->Cymbal, `bells`->Water Bells, `horn5`->Engl Horn. Those are
// somebody's choices made by ear in the 1990s, and they beat any rule we could
// write - so when a song carries them they are the default this player offers.
//
// Most songs carry nothing: a flag of 1 means tone 0, which is `Acou Piano1`,
// the untouched default every slot starts at. Only 10 .GYB and 2 .OKA in the
// owner's library were ever assigned.
#pragma once

namespace mt32map {

// 1..128, as the original numbers them. Out of range returns "".
const char* toneName(int tone);

struct GmChoice {
    bool drum = false;      // route to GM channel 10
    int  program = 0;       // GM program, 0-based, when melodic
    int  drumNote = 38;     // GM percussion note when drum
    bool exact = true;      // false where the equivalent is a judgement call
};

// Convert an MT-32 tone number (1..128) to its General MIDI equivalent.
GmChoice toGeneralMidi(int tone);

// The other direction, for the F5 dialog's MT-32 mode: given a GM program
// (0-based), the first MT-32 tone that maps to it, or 0 when none does.
//
// Derived by inverting the table above rather than written by hand. That keeps
// the two directions from disagreeing, and means no new judgement calls are
// introduced - a row that GM matched by name lands on a tone the same table
// already says is its equivalent, instead of dropping to tone 1.
//
// Not exhaustive by construction: the MT-32 has no equivalent for parts of the
// GM set, and those return 0 so the caller can fall back. Melodic only -
// percussion is handled by the drum-note path.
int fromGeneralMidi(int gmProgram);

// Decode a .GYB / .OKA instrument record flag byte.
inline int  toneOfFlag(unsigned char flag) { return (flag >> 1) + 1; }  // 1..128
inline bool flagHasParams(unsigned char flag) { return (flag & 1) != 0; }
// A song that was never assigned leaves tone 1 everywhere, so tone 1 carries no
// information and callers should fall back to matching the OPL name instead.
inline bool flagCarriesAssignment(unsigned char flag) { return (flag >> 1) != 0; }

}  // namespace mt32map
