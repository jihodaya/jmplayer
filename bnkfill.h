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
QHash<QString, QByteArray> loadInstrumentBank(const QString& songPath, int paramLen,
                                              QString* chosenPath = nullptr);

int fillEmptyInstrumentSlots(const QStringList& slotNames,
                             QList<QByteArray>& params,
                             const QString& songPath,
                             int paramLen,
                             const char* logTag);

#endif // BNKFILL_H
