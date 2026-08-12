#include "bnkfill.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>

QHash<QString, QByteArray> loadInstrumentBank(const QString& songPath, int paramLen,
                                              QString* chosenPath)
{
    // STAND.BNK first: it is byte-identical to the bank that shipped with the
    // DOS players themselves (md5 b073529e...), so songs get the instruments
    // their own player loaded. The larger STANDARD.BNK we ship for IMS/ROL is a
    // different collection and only the fallback.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QFileInfo songInfo(songPath);
    // The program's own bank comes FIRST, the way GAYOBANG and NORE45 read the
    // bank sitting next to themselves rather than one next to the song. A song
    // folder can hold an unrelated STANDARD.BNK - D:/.../test/76 does, and its
    // BELLS is the dull variant - which would quietly win otherwise.
    QStringList candidates;
    candidates << appDir + "/STAND.BNK"
               << appDir + "/STANDARD.BNK"
               << appDir + "/dos/STANDARD.BNK";
    if (!songPath.isEmpty())
        candidates << songInfo.absolutePath() + "/STAND.BNK"
                   << songInfo.absolutePath() + "/STANDARD.BNK"
                   << songInfo.absolutePath() + "/standard.bnk";
    candidates << "D:/py/mt32-extend/jmp/IMS/STAND.BNK"       // dev tree
               << "D:/py/mt32-extend/jmp/IMS/STANDARD.BNK";

    QByteArray raw;
    QString chosen;
    for (const QString& c : candidates) {
        QFile f(c);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) continue;
        raw = f.readAll();
        chosen = c;
        break;
    }
    QHash<QString, QByteArray> byName;
    if (raw.size() < 20) return byName;
    if (chosenPath) *chosenPath = chosen;

    // .BNK: total entries at 10, name-list and data offsets at 12 and 16. A name
    // entry is index u16 + used u8 + 9 chars; a data entry is 30 bytes whose
    // first two (mode, voice number) are not part of the instrument.
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

int fillEmptyInstrumentSlots(const QStringList& slotNames,
                             QList<QByteArray>& params,
                             const QString& songPath,
                             int paramLen,
                             const char* logTag)
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

    QString chosen;
    const QHash<QString, QByteArray> byName =
        loadInstrumentBank(songPath, paramLen, &chosen);
    if (byName.isEmpty()) {
        qWarning() << logTag << empty.size()
                   << "slots carry names only and no bank was found - silent";
        return 0;
    }

    int hit = 0;
    for (int i : empty) {
        auto it = byName.constFind(slotNames[i].trimmed().toUpper());
        if (it == byName.constEnd()) continue;
        params[i] = it.value();
        ++hit;
    }
    qDebug() << logTag << "name-only slots:" << empty.size()
             << "resolved" << hit << "from" << chosen;
    return hit;
}
