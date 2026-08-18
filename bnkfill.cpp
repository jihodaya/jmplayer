#include "bnkfill.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <cstdlib>

// Parse one .BNK into a name -> 28-parameter-byte map. Empty if the file is
// missing or does not parse.
//
// Layout: total entries at 10, name-list and data offsets at 12 and 16. A name
// entry is index u16 + used u8 + 9 chars; a data entry is 30 bytes whose first
// two (mode, voice number) are not part of the instrument.
static QHash<QString, QByteArray> parseBankFile(const QString& path, int paramLen)
{
    QHash<QString, QByteArray> byName;
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return byName;
    const QByteArray raw = f.readAll();
    if (raw.size() < 20) return byName;

    const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.constData());
    auto u16 = [&](int o) { return (int)(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](int o) {
        return (int)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24));
    };
    const int total = u16(10), nameOff = u32(12), dataOff = u32(16);
    for (int i = 0; i < total; ++i) {
        const int p = nameOff + i * 12;
        if (p + 12 > raw.size()) break;
        const int q = dataOff + u16(p) * 30;
        if (q + 30 > raw.size()) continue;
        QByteArray nm(reinterpret_cast<const char*>(d + p + 3), 9);
        const int nul = nm.indexOf(char(0));
        if (nul >= 0) nm.truncate(nul);
        byName.insert(QString::fromLatin1(nm).trimmed().toUpper(),
                      QByteArray(reinterpret_cast<const char*>(d + q + 2), paramLen));
    }
    return byName;
}

// Kept for callers that just want "whatever bank is around" - the fill below
// uses parseBankFile() directly so it can walk an ordered chain.
QHash<QString, QByteArray> loadInstrumentBank(const QString& songPath, int paramLen,
                                              QString* chosenPath)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + "/GAYO.BNK" << appDir + "/NORE.BNK"
               << appDir + "/STANDARD.BNK"
               << "D:/py/mt32-extend/jmp/IMS/GAYO.BNK"
               << "D:/py/mt32-extend/jmp/IMS/NORE.BNK";
    if (!songPath.isEmpty())
        candidates << QFileInfo(songPath).absolutePath() + "/STANDARD.BNK";
    for (const QString& c : candidates) {
        QHash<QString, QByteArray> m = parseBankFile(c, paramLen);
        if (m.isEmpty()) continue;
        if (chosenPath) *chosenPath = c;
        return m;
    }
    return {};
}

const QByteArray& builtinDefaultInstrument(int index)
{
    // Byte for byte out of GAYOBANG.EXE at 0x445a:0x120a, six 28-byte records.
    static const unsigned char kDefaults[6][28] = {
        { 0x01,0x01,0x03,0x0f,0x05,0x00,0x01,0x03,0x0f,0x00,0x00,0x00,0x01,
          0x00,0x01,0xf6,0x0d,0x0f,0x00,0x02,0x02,0x00,0x00,0x00,0x01,0x01, 0x00,0x00 },
        { 0x00,0x00,0x00,0x0a,0x04,0x00,0x08,0x0c,0x0b,0x00,0x00,0x00,0x01,
          0x00,0x00,0x2f,0x0d,0x04,0x00,0x06,0x0f,0x00,0x00,0x00,0x00,0x01, 0x00,0x00 },
        { 0x00,0x0c,0x00,0x0f,0x0b,0x00,0x08,0x05,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x2f,0x0d,0x04,0x00,0x06,0x0f,0x00,0x00,0x00,0x00,0x00, 0x00,0x00 },
        { 0x00,0x04,0x00,0x0f,0x0b,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x2f,0x0d,0x04,0x00,0x06,0x0f,0x00,0x00,0x00,0x00,0x00, 0x00,0x00 },
        { 0x00,0x01,0x00,0x0f,0x0b,0x00,0x05,0x05,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x2f,0x0d,0x04,0x00,0x06,0x0f,0x00,0x00,0x00,0x00,0x00, 0x00,0x00 },
        { 0x00,0x01,0x00,0x0f,0x0b,0x00,0x07,0x05,0x00,0x00,0x00,0x00,0x00,
          0x00,0x00,0x2f,0x0d,0x04,0x00,0x06,0x0f,0x00,0x00,0x00,0x00,0x00, 0x00,0x00 }
    };
    static QByteArray cache[6];
    if (cache[0].isEmpty())
        for (int i = 0; i < 6; ++i)
            cache[i] = QByteArray(reinterpret_cast<const char*>(kDefaults[i]), 28);
    if (index < 0 || index > 5) index = 0;
    return cache[index];
}

int fillEmptyInstrumentSlots(const QStringList& slotNames,
                             QList<QByteArray>& params,
                             const QString& songPath,
                             int paramLen,
                             const char* logTag,
                             BankOrder order,
                             const QString& externalBank)
{
    QList<int> empty;
    for (int i = 0; i < params.size() && i < slotNames.size(); ++i) {
        const QByteArray& p = params[i];
        bool zero = true;
        for (int k = 0; k < p.size(); ++k) if (p[k] != 0) { zero = false; break; }
        if (zero && !slotNames[i].trimmed().isEmpty())
            empty.append(i);
    }
    if (empty.isEmpty()) return 0;

    // Its own program's bank first, the other program's second, and then the
    // patch the player carries inside itself. Nothing else: those two are the
    // only banks the DOS programs ever read, so consulting a third would find
    // instruments neither of them could - THDRFOS1's `bassbel3` is exactly
    // that case, and reading it out of the large IMS/ROL collection gave a
    // thin, bright voice where the original substitutes its own patch.
    //
    // GAYO.BNK is GAYOBANG's (2,154 instruments) and NORE.BNK is NORE45's
    // (6,009). The larger carries all but one of the smaller's names, but 244
    // of the shared ones hold different parameters, so which comes first
    // decides the voice. STANDARD.BNK - the 15,867-instrument general OPL
    // collection - is deliberately NOT here: it belongs to the .IMS/.ROL side,
    // neither DOS program ever read it, and pulling THDRFOS1's `bassbel3` out
    // of it gave a thin, bright voice the original never produced.
    const QStringList files = (order == BankOrder::Gayobang)
        ? QStringList{ "GAYO.BNK", "NORE.BNK" }
        : QStringList{ "NORE.BNK", "GAYO.BNK" };

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString songDir = songPath.isEmpty() ? QString()
                                               : QFileInfo(songPath).absolutePath();

    QList<QPair<QString, QHash<QString, QByteArray>>> chain;

    // The bank the user picked with the BNK button comes before everything.
    // It used to reach only resolveBnkPatches(), which runs solely when the
    // embedded load fails outright, so on .GYB / .OKA the button did almost
    // nothing - the slots it was meant to influence had already been filled
    // here.
    if (!externalBank.isEmpty()) {
        QHash<QString, QByteArray> m = parseBankFile(externalBank, paramLen);
        if (!m.isEmpty()) chain.append({ externalBank, m });
    }

    for (const QString& file : files) {
        QStringList where;
        // The program's own bank comes before the song folder's: a song folder
        // can hold an unrelated STANDARD.BNK - test/76 does, and its BELLS is
        // the dull variant - which would quietly win otherwise.
        where << appDir + "/" + file
              << "D:/py/mt32-extend/jmp/IMS/" + file;      // dev tree
        if (!songDir.isEmpty()) where << songDir + "/" + file;
        for (const QString& w : where) {
            QHash<QString, QByteArray> m = parseBankFile(w, paramLen);
            if (m.isEmpty()) continue;
            chain.append({ w, m });
            break;
        }
    }

    int hit = 0;
    QStringList used;
    for (int i : empty) {
        const QString key = slotNames[i].trimmed().toUpper();
        bool got = false;
        for (const auto& b : chain) {
            auto it = b.second.constFind(key);
            if (it == b.second.constEnd()) continue;
            params[i] = it.value();
            ++hit; got = true;
            if (!used.contains(b.first)) used << b.first;
            break;
        }
        // Nothing carried it, and the record stays at zero on purpose: the
        // channel keeps the instrument it was already playing.
        //
        // The original does NOT do this - GAYOBANG loads built-in patch 0 - and
        // GybBackend::loadEmbeddedPatches() carries the full note on why the
        // measurement went the other way. This is a rare path: across the 169
        // .GYB/.OKA files here, 96 songs carry 1,121 empty slots and the bank
        // chain resolves 1,113 of them. Eight fall through, in four songs
        // (THDRFOS1, STATION.GYB/.OKA, NOISENEW.OKA), and only THDRFOS1's
        // `bassbel3` has ever been compared against a real machine.
        //
        // Substituting a patch was measured and is worse. Against the DOS
        // capture of THDRFOS1, band-profile distance over 160 Hz - 5.1 kHz:
        // 2.59 dB keeping the previous patch, 4.91 dB filling from the big
        // 15,867-instrument STANDARD.BNK, 5.24 dB from GAYOBANG's own built-in.
        // Both substitutes run 6.7-8.4 dB hot at 2.5-5 kHz - the thin, bright
        // voice the owner also heard - where the DOS machine keeps the
        // preceding `sc-pin2`. That is why STANDARD.BNK is not in the chain
        // above and why nothing is loaded here.
        //
        // The built-in patches are still used - as the channels' initial state,
        // which is where FUN_255a_101b applies them.
    }
    qDebug() << logTag << "name-only slots:" << empty.size()
             << "resolved" << hit << "from" << used
             << "left to the channel's previous instrument:" << (empty.size() - hit);
    return hit;
}
