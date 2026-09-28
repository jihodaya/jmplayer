#ifndef MDXMIDI_H
#define MDXMIDI_H

#include <QByteArray>
#include <QString>
#include <vector>

// `.mdz` - the compiled data format of MLD, the X68000 OPM / ADPCM / MIDI
// driver by LUM2 (1990-93). Not MDX, despite the family resemblance: MLD's own
// documentation calls itself "MXDRV + MDD like", so the title, the 0D 0A 1A,
// the name, the NUL and the big-endian offset table all match while the
// commands do not.
//
//     0x00-0x7F   a note, followed by its duration in ticks
//     0x80        a rest, followed by its duration
//     0x81-0xFF   commands
//
// Everything about the encoding was settled by compiling MML with MLD's own
// `mlc.x` under run68 and reading the bytes - see jmp/CLAUDE.md section 7-7.
// `A C1 V105 @v115 p64 @16 @t228 y91,40 o4 c4 r4` compiles to
// `E1 10 E3 16 FB 8C FC 40 FD 10 FF E4 FE 5B 28 3C 30 80 30`, which fixes the
// note encoding, the 48-tick quarter and six commands in one line.
namespace mdxmidi {

// Does this file route any track to a MIDI module? A static scan - no
// emulation. `C1` (a MIDI channel) compiles to `E1 10` and `CH1` (an OPM
// channel) to `E1 00`, so bit 4 of that operand is the whole distinction.
bool hasMidiTracks(const QString& path);

// The MIDI-routed tracks as SMF bytes, or empty if there are none.
QByteArray toMidi(const QString& path);

// Header: the title, then a big-endian table whose first entry is the voice
// table and the rest one offset per track, all relative to `base`. A few files
// sit some bytes further on, so the base is walked forward until the table it
// describes is coherent. Shared with the FM engine, which reads the same file.
bool parseHeader(const unsigned char* d, int n, int& base, std::vector<int>& trackOffsets,
                 int& voiceOffset);

// How many operand bytes a command takes, or -1 where it is not established.
// ONE table, because the FM and MIDI halves of a song are the same byte
// stream - only the interpretation differs.
int operandCount(unsigned char c);

// Commands whose operands run to an 0xF7 terminator instead of a fixed count:
// the MML `EX` / `WEX` exclusives and the `#SC:` / `#MT:` module macros.
bool isBlobCommand(unsigned char c);

// The operand of a track's first channel command (0xE1), or -1 when it names
// none. Exclusives are skipped whole on the way, because a track often opens
// with long module setup - NEW_Wa's first track sends 1.5 KB of CM-32L timbres
// before its CHP1 - and a fixed window of bytes stopped inside them.
int firstChannelOperand(const unsigned char* d, int n, int start, int end);

// The documented v# -> @v# curve for the coarse velocity form.
int velocityCurve(int index);

} // namespace mdxmidi

#endif
