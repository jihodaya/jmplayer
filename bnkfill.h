#ifndef BNKFILL_H
#define BNKFILL_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QHash>

// Fill instrument records the song left as name-only, by looking the name up in
// a bank.
//
// A .GYB / .OKA instrument record is 9 bytes of name, a flag byte, then 28
// parameter bytes. Bit 0 of the flag says whether those 28 bytes are real: over
// 772 records in the test folders the correlation is exact - flag set means
// parameters, flag clear means all 28 are zero. Files converted from .ROL carry
// names only, and playing their zeros programs an operator with attack rate 0,
// which never rises, so the whole song is silent. NORE45 and GAYOBANG resolve
// such names from STANDARD.BNK (NORE45.EXE.c FUN_1c24_012e is a binary search
// over the bank's name list), and this does the same.
//
// It touches ONLY the empty records. A song that carries its own parameters -
// the overwhelming majority - never consults a bank at all, which is what
// playing the DOS players with their bank deleted shows they do.
//
// Returns the number of slots filled. `params` is modified in place.
// Load a .BNK as a name -> 28-parameter-byte map. Empty if none was found.
// Which program's bank a song should be read against first. GAYOBANG and
// NORE45 shipped different banks: 2,154 instruments against 6,005, and although
// the larger one carries all but one of the smaller one's names, 244 of the
// shared names hold different parameters. A .GYB read against NORE45's bank
// therefore plays 244 possible instruments in the wrong voice, and vice versa.
enum class BankOrder { Gayobang, Nore45 };

// Load a .BNK as a name -> 28-parameter-byte map. Empty if none was found.
QHash<QString, QByteArray> loadInstrumentBank(const QString& songPath, int paramLen,
                                              QString* chosenPath = nullptr);

// The six patches the DOS players carry inside themselves, at DS:0x120a in
// GAYOBANG.EXE and DS:0x5F3 in NORE45.EXE: index 0 is the melodic default and
// 1..5 the rhythm kit for channels 6..10. A record left empty falls back to one
// of these - `FUN_255a_016b(ch, idx * 0x1c + 0x120a)` with
// `idx = (!rhythm || ch < 6) ? 0 : ch - 5` - which is the last tier, after the
// song's own parameters and a bank lookup by name. Verified against the bytes
// extracted from GAYOBANG.EXE.
const QByteArray& builtinDefaultInstrument(int index);

int fillEmptyInstrumentSlots(const QStringList& slotNames,
                             QList<QByteArray>& params,
                             const QString& songPath,
                             int paramLen,
                             const char* logTag,
                             BankOrder order,
                             const QString& externalBank = QString());

#endif // BNKFILL_H
