#ifndef SNGMIDI_H
#define SNGMIDI_H

#include <QByteArray>
#include <QString>

// `.SNG` - Ballade, the PC-9801 MIDI sequencer Dynaware sold through the
// 1990s, and the ミュージくん / ミュージ郎 packages built on it. Nothing to do
// with Recomposer, which also uses the extension: a Ballade file says so in
// its first line and jmp used to refuse it on that basis (see
// RcpFileHandler::isRcpData).
//
// The first line names the program that wrote the file, and that is the only
// version marker there is:
//
//     BALLADE SONG Ver. 1.00   Ballade      / ミュージくん   10 parts
//     BALLADE SONG Ver. 2.00   Ballade2     / ミュージ郎     10 parts
//     BALLADE SONG Ver. 3.00   ミュージ郎2                   10 parts
//     BALLADE SONG Ver. 4.00   Ballade3                      16 parts
//
// The file is a dump of the editor's heap, not a stream: a fixed header, a
// table of block sizes, then one block per part, each holding two parallel
// event areas (notes, and controllers) plus slack. 48 ticks to the quarter,
// and the song proper starts one bar in - Ballade counts that bar off.
//
// Everything here was settled against SNG2S 3.3, M. Saito's 1993 DOS converter,
// run under msdos-player over all seventeen sample files; fifteen of them come
// out note-for-note identical. See jmp/CLAUDE.md section 7-8.
namespace sngmidi {

// Is this Ballade? The signature is the literal first line, so this is exact -
// no heuristics, and no chance of claiming a Recomposer .sng.
bool isSngData(const QByteArray& data);
bool isSngFile(const QString& path);

// The song name, Shift-JIS, from the second line of the header.
QString extractTitle(const QByteArray& data);
QString extractTitle(const QString& path);

// The whole song as a Standard MIDI File, or empty if the file is not Ballade
// or its block table does not describe the bytes that are actually there.
QByteArray toMidi(const QString& path);
QByteArray toMidi(const QByteArray& data);

} // namespace sngmidi

#endif // SNGMIDI_H
