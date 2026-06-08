#include "nobfilehandler.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif
#include <QDebug>
#include <QDateTime>
#include <windows.h>
#include <cmath>

namespace {
QString externalLyricsFilePath(const QString& filePath)
{
    QFileInfo info(filePath);
    if (!info.exists()) {
        return QString();
    }
    if (info.suffix().compare(QStringLiteral("nob"), Qt::CaseInsensitive) != 0) {
        return QString();
    }
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".txt");
}
} // namespace



bool NobFileHandler::isNobFile(const QString& filePath)

{

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        return false;

    }



    // ?ㅻ뜑 ?쎄린

    QByteArray header = file.read(NOB_HEADER_SIZE);

    if (header.size() < NOB_HEADER_SIZE) {

        return false;

    }



    // ?뚮옒洹??뺤씤 (0x00 == 0x08)

    if (static_cast<unsigned char>(header[0]) != NOB_FLAG) {

        return false;

    }



    // MIDI ?ㅻ뜑 ?뺤씤 (0x45遺??"MThd")

    file.seek(MIDI_OFFSET);

    QByteArray midiHeader = file.read(4);



    return (midiHeader == "MThd");

}



QString NobFileHandler::extractTitle(const QString& filePath)

{

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        return QString();

    }



    // ?ㅻ뜑 ?쎄린

    QByteArray header = file.read(NOB_HEADER_SIZE);

    if (header.size() < NOB_HEADER_SIZE) {

        return QString();

    }



    return extractTitleFromHeader(header);

}



QByteArray NobFileHandler::extractMidiData(const QString& filePath)

{

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        return QByteArray();

    }



    // MIDI ?ㅽ봽?뗭쑝濡??대룞

    if (!file.seek(MIDI_OFFSET)) {

        qWarning() << "Failed to seek to MIDI offset in NOB file:" << filePath;

        return QByteArray();

    }



    // ?섎㉧吏 ?꾨?媛 MIDI ?곗씠??

    QByteArray midiData = file.readAll();



    if (midiData.isEmpty() || !midiData.startsWith("MThd")) {

        qWarning() << "Invalid MIDI data in NOB file:" << filePath;

        return QByteArray();

    }



    qDebug() << "Extracted MIDI data from NOB:" << midiData.size() << "bytes";

    return midiData;

}



QString NobFileHandler::extractTitleFromHeader(const QByteArray& header)

{

    if (header.size() < NOB_HEADER_SIZE) {

        return QString();

    }



    // 0x01-0x0F ?곸뿭 異붿텧 (理쒕? 15 bytes)

    QByteArray titleBytes = header.mid(NOB_TITLE_OFFSET, NOB_TITLE_SIZE);



    // NULL 諛붿씠???꾧퉴吏留?異붿텧 (C 臾몄옄??諛⑹떇)

    int actualLen = 0;

    for (int i = 0; i < titleBytes.size(); ++i) {

        if (titleBytes[i] == 0) {

            break;

        }

        actualLen++;

    }



    if (actualLen == 0) {

        return QString(); // 鍮??쒕ぉ

    }



    // Windows API瑜??ъ슜??議고빀???쒓?(Johab, CP1361) ?붿퐫??

    const char* src = titleBytes.constData();



    // MultiByteToWideChar濡??꾩슂??踰꾪띁 ?ш린 怨꾩궛 (actualLen留뚰겮留?

    // 1361 = Johab (議고빀???쒓?)

    int wideLen = MultiByteToWideChar(1361, 0, src, actualLen, NULL, 0);

    if (wideLen <= 0) {

        qWarning() << "Failed to decode Johab title, actualLen:" << actualLen;

        return QString();

    }



    // Wide character 踰꾪띁 ?좊떦 諛?蹂??

    wchar_t* wideBuf = new wchar_t[wideLen + 1];

    MultiByteToWideChar(1361, 0, src, actualLen, wideBuf, wideLen);

    wideBuf[wideLen] = 0;



    QString title = QString::fromWCharArray(wideBuf);

    delete[] wideBuf;



    // 怨듬갚 ?쒓굅

    title = title.trimmed();



    qDebug() << "Extracted NOB title:" << title << "(raw bytes:" << actualLen << ")";

    return title;

}



QString NobFileHandler::extractTitleFromLst(const QString& nobFilePath)

{

    QFileInfo nobInfo(nobFilePath);

    QString nobDir = nobInfo.absolutePath();

    QString nobFileName = nobInfo.fileName().toUpper(); // 대소문자 무시



    // LST 파일 경로 확인

    QString lstPath;

    if (QFile::exists(nobDir + "/NOB.LST")) {

        lstPath = nobDir + "/NOB.LST";

    } else if (QFile::exists(nobDir + "/nob.lst")) {

        lstPath = nobDir + "/nob.lst";

    } else if (QFile::exists(nobDir + "/Nob.lst")) {

        lstPath = nobDir + "/Nob.lst";

    } else {

        return QString();

    }



    // static 변수를 이용해 마지막으로 오픈 및 분석한 LST 캐시 보관

    static QString s_lastLstPath;

    static QList<QByteArray> s_lastLstEntries;

    static QDateTime s_lastLstModifiedTime;



    QFileInfo lstInfo(lstPath);

    QDateTime currentModifiedTime = lstInfo.lastModified();



    // LST 파일 경로가 달라졌거나 파일이 갱신되었을 때만 디스크에서 새로 분석

    if (s_lastLstPath != lstPath || s_lastLstModifiedTime != currentModifiedTime) {

        QFile lstFile(lstPath);

        if (!lstFile.open(QIODevice::ReadOnly)) {

            return QString();

        }

        QByteArray lstData = lstFile.readAll();

        lstFile.close();



        s_lastLstEntries = lstData.split('\x0A');

        s_lastLstPath = lstPath;

        s_lastLstModifiedTime = currentModifiedTime;

    }



    for (const QByteArray& entry : s_lastLstEntries) {

        // .NOB ?뚯씪紐?李얘린

        int nobIdx = entry.toUpper().indexOf(".NOB");

        if (nobIdx < 0) continue;



        // ?뚯씪紐??쒖옉 ?꾩튂 李얘린 (??갑?μ쑝濡?NULL???꾨땶 臾몄옄 李얘린)

        int start = nobIdx;

        while (start > 0 && entry[start-1] != '\x00') {

            start--;

        }



        // ?뚯씪紐?異붿텧

        QByteArray filenameBytes = entry.mid(start, nobIdx - start + 4);

        QString filename = QString::fromLatin1(filenameBytes).trimmed().toUpper();



        // ?꾩옱 NOB ?뚯씪怨?留ㅼ묶?섎뒗吏€ ?뺤씤

        if (filename == nobFileName) {

            // ?쒕ぉ 異붿텧 (?뚯씪紐??댁쟾??紐⑤뱺 non-null 諛붿씠??

            QByteArray titleBytes;

            for (int i = 0; i < start; ++i) {

                if (entry[i] != '\x00' && entry[i] != '\x0D') {

                    titleBytes.append(entry[i]);

                }

            }



            if (titleBytes.isEmpty()) {

                return QString();

            }



            // 議고빀???쒓?(CP1361) ?붿퐫??

            const char* src = titleBytes.constData();

            int srcLen = titleBytes.size();



            int wideLen = MultiByteToWideChar(1361, 0, src, srcLen, NULL, 0);

            if (wideLen <= 0) {

                return QString();

            }



            wchar_t* wideBuf = new wchar_t[wideLen + 1];

            MultiByteToWideChar(1361, 0, src, srcLen, wideBuf, wideLen);

            wideBuf[wideLen] = 0;



            QString title = QString::fromWCharArray(wideBuf);

            delete[] wideBuf;



            title = title.trimmed();

            qDebug() << "Extracted LST title for" << nobFileName << ":" << title;

            return title;

        }

    }



    return QString(); // 留ㅼ묶?섎뒗 ??ぉ ?놁쓬

}



QStringList NobFileHandler::extractLyrics(const QString& filePath)

{

    QStringList lyrics;



    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        qDebug() << "Failed to open NOB file for lyrics:" << filePath;

        return lyrics;

    }



    QByteArray data = file.readAll();

    file.close();



    if (data.size() < MIDI_OFFSET) {

        qDebug() << "NOB file too small for lyrics";

        return lyrics;

    }



    // 1. 留덉?留?MIDI ?몃옓 ??李얘린 (FF 2F 00 = End of Track)

    QByteArray eotPattern;

    eotPattern.append((char)0xFF);

    eotPattern.append((char)0x2F);

    eotPattern.append((char)0x00);



    int midiEnd = data.lastIndexOf(eotPattern);

    if (midiEnd == -1) {

        qDebug() << "MIDI End of Track not found";

        return lyrics;

    }



    midiEnd += 3; // FF 2F 00 ?ㅼ쓬 ?꾩튂

    qDebug() << "MIDI ends at offset:" << midiEnd << "/ File size:" << data.size();



    // 2. NULL ?⑤뵫 嫄대꼫?곌린 (100+ bytes)

    int lyricStart = midiEnd;

    int nullCount = 0;



    for (int i = midiEnd; i < data.size(); ++i) {

        if (data[i] == 0x00) {

            nullCount++;

        } else {

            if (nullCount > 100) {

                lyricStart = i;

                break;

            }

            nullCount = 0;

        }

    }



    qDebug() << "Lyric data starts at offset:" << lyricStart;



    if (lyricStart >= data.size()) {

        qDebug() << "No lyrics data found (lyricStart >= data.size)";

        return lyrics;

    }



    // 3. 媛???곗씠??異붿텧

    QByteArray lyricData = data.mid(lyricStart);



    // 4. 以??⑥쐞濡?遺꾨━ (5+ ?곗냽 NULL = 以꾨컮轅? 5媛?誘몃쭔 NULL = 湲댁쓬 留덉빱)

    QByteArray currentLine;

    QByteArray pendingNulls; // 5媛?誘몃쭔??NULL ?꾩떆 ???

    nullCount = 0;



    // ?붾쾭洹? 泥섏쓬 500 諛붿씠?몄쓽 16吏꾩닔 ?ㅽ봽 + NULL ?곗냽 ?⑦꽩 遺꾩꽍

    qDebug() << "[NobFileHandler] === Lyric Data Analysis ===";

    qDebug() << "Total lyric data size:" << lyricData.size() << "bytes";



    // NULL ?곗냽 ?⑦꽩 遺꾩꽍

    qDebug() << "[NobFileHandler] NULL sequence analysis:";

    int consecutiveNulls = 0;

    QList<int> nullSequences;

    for (int i = 0; i < qMin(500, lyricData.size()); ++i) {

        if (lyricData[i] == 0x00) {

            consecutiveNulls++;

        } else {

            if (consecutiveNulls > 0) {

                nullSequences.append(consecutiveNulls);

                qDebug() << "  Position" << (i - consecutiveNulls) << ": " << consecutiveNulls << "consecutive NULLs";

                consecutiveNulls = 0;

            }

        }

    }



    qDebug() << "[NobFileHandler] === First 500 bytes hex dump ===";

    QString hexDump;

    int dumpLimit = qMin(500, lyricData.size());

    for (int i = 0; i < dumpLimit; ++i) {

        unsigned char b = static_cast<unsigned char>(lyricData[i]);

        hexDump += QString::number(b, 16).rightJustified(2, '0') + " ";

        if ((i + 1) % 30 == 0) {

            qDebug() << hexDump;

            hexDump.clear();

        }

    }

    if (!hexDump.isEmpty()) {

        qDebug() << hexDump;

    }



    for (int i = 0; i < lyricData.size(); ++i) {

        unsigned char byte = static_cast<unsigned char>(lyricData[i]);



        if (byte == 0x00) {

            nullCount++;

            pendingNulls.append((char)0x00);

        } else {

            // Non-NULL 諛붿씠??

            if (nullCount > 0 && nullCount < 5) {

                // 5媛?誘몃쭔??NULL? 湲댁쓬 留덉빱濡?異붽?

                currentLine.append(pendingNulls);

            } else if (nullCount >= 5) {

                // 5媛??댁긽 NULL = 以꾨컮轅?

                if (!currentLine.isEmpty()) {

                    QString decoded = decodeJohab(currentLine);

                    if (!decoded.isEmpty()) {

                        lyrics.append(decoded);

                    }

                    currentLine.clear();

                }

            }



            // ?꾩옱 諛붿씠??異붽?

            currentLine.append(byte);

            nullCount = 0;

            pendingNulls.clear();

        }

    }



    // 留덉?留?以?泥섎━

    if (!currentLine.isEmpty()) {

        QString decoded = decodeJohab(currentLine);

        if (!decoded.isEmpty()) {

            lyrics.append(decoded);

        }

    }



    qDebug() << "Extracted" << lyrics.size() << "lyric lines from NOB file";



    // ?붾쾭洹? 泥섏쓬 3以?異쒕젰

    if (!lyrics.isEmpty()) {

        qDebug() << "First lyric line:" << lyrics[0];

        if (lyrics.size() > 1) qDebug() << "Second lyric line:" << lyrics[1];

        if (lyrics.size() > 2) qDebug() << "Third lyric line:" << lyrics[2];

    }



    return lyrics;

}




QStringList NobFileHandler::loadExternalLyrics(const QString& filePath, bool *found)
{
    if (found) {
        *found = false;
    }

    const QString txtPath = externalLyricsFilePath(filePath);
    if (txtPath.isEmpty()) {
        return QStringList();
    }

    QFile file(txtPath);
    if (!file.exists()) {
        return QStringList();
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[NobFileHandler] Failed to open external lyrics file:" << txtPath;
        return QStringList();
    }

    if (found) {
        *found = true;
    }

    QTextStream in(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    in.setEncoding(QStringConverter::Utf8);
#else
    in.setCodec("UTF-8");
#endif
    QString content = in.readAll();
    file.close();

    content.replace("\r\n", "\n");
    content.replace('\r', '\n');

    return content.split('\n', Qt::KeepEmptyParts);
}

bool NobFileHandler::saveExternalLyrics(const QString& filePath, const QStringList& lyrics)
{
    const QString txtPath = externalLyricsFilePath(filePath);
    if (txtPath.isEmpty()) {
        return false;
    }

    QSaveFile file(txtPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[NobFileHandler] Failed to open lyrics file for writing:" << txtPath;
        return false;
    }

    QTextStream out(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    out.setEncoding(QStringConverter::Utf8);
#else
    out.setCodec("UTF-8");
#endif
    for (int i = 0; i < lyrics.size(); ++i) {
        out << lyrics.at(i);
        if (i < lyrics.size() - 1) {
            out << '\n';
        }
    }

    if (!file.commit()) {
        qWarning() << "[NobFileHandler] Failed to commit lyrics file:" << txtPath;
        return false;
    }

    return true;
}unsigned long NobFileHandler::calculateTotalTicks(const QString& filePath)

{

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        qDebug() << "Failed to open NOB file for tick calculation:" << filePath;

        return 0;

    }



    QByteArray data = file.readAll();

    file.close();



    // MIDI ?곗씠??異붿텧 (0x45遺???쒖옉)

    if (data.size() < MIDI_OFFSET + 14) {

        return 0;

    }



    QByteArray midiData = data.mid(MIDI_OFFSET);



    // MThd ?ㅻ뜑 ?뺤씤

    if (midiData.left(4) != "MThd") {

        return 0;

    }



    // 紐⑤뱺 ?몃옓???뚯떛?섏뿬 媛??湲??쒓컙 李얘린

    unsigned long maxTicks = 0;



    // MTrk 泥?겕??李얘린

    int pos = 0;

    while (pos < midiData.size()) {

        int mtrkPos = midiData.indexOf("MTrk", pos);

        if (mtrkPos == -1) break;



        // ?몃옓 湲몄씠 ?쎄린 (鍮낆뿏?붿븞)

        if (mtrkPos + 8 > midiData.size()) break;



        unsigned long trackLen = ((unsigned char)midiData[mtrkPos + 4] << 24) |

                                 ((unsigned char)midiData[mtrkPos + 5] << 16) |

                                 ((unsigned char)midiData[mtrkPos + 6] << 8) |

                                 ((unsigned char)midiData[mtrkPos + 7]);



        // ?몃옓 ?곗씠??

        int trackDataStart = mtrkPos + 8;

        if (trackDataStart + trackLen > (unsigned long)midiData.size()) {

            break;

        }



        // ???몃옓??珥??쒓컙 怨꾩궛

        unsigned long currentTime = 0;

        int offset = trackDataStart;

        int trackEnd = trackDataStart + trackLen;

        unsigned char runningStatus = 0;



        while (offset < trackEnd) {

            // ?명? ????쎄린 (媛蹂 湲몄씠)

            unsigned long deltaTime = 0;

            while (offset < trackEnd) {

                unsigned char byte = midiData[offset++];

                deltaTime = (deltaTime << 7) | (byte & 0x7F);

                if (!(byte & 0x80)) break;

            }

            currentTime += deltaTime;



            if (offset >= trackEnd) break;



            // ?대깽?????

            unsigned char status = midiData[offset];



            if (status < 0x80) {

                // Running status

                if (runningStatus == 0) {

                    offset++;

                    continue;

                }

                status = runningStatus;

            } else {

                offset++;

                runningStatus = status;

            }



            // ?대깽??泥섎━ (湲몄씠留?怨꾩궛?섏뿬 ?ㅽ궢)

            if (status == 0xFF) {

                // Meta event

                if (offset >= trackEnd) break;

                unsigned char metaType = midiData[offset++];



                // 湲몄씠 ?쎄린

                unsigned long length = 0;

                while (offset < trackEnd) {

                    unsigned char byte = midiData[offset++];

                    length = (length << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) break;

                }



                offset += length;



                // End of Track

                if (metaType == 0x2F) {

                    break;

                }

            } else if (status == 0xF0 || status == 0xF7) {

                // SysEx

                unsigned long length = 0;

                while (offset < trackEnd) {

                    unsigned char byte = midiData[offset++];

                    length = (length << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) break;

                }

                offset += length;

            } else {

                // Channel event

                unsigned char eventType = status & 0xF0;

                if (eventType == 0xC0 || eventType == 0xD0) {

                    // 1 data byte

                    offset += 1;

                } else {

                    // 2 data bytes

                    offset += 2;

                }

            }

        }



        // ???몃옓??理쒕? ?쒓컙 ?낅뜲?댄듃

        if (currentTime > maxTicks) {

            maxTicks = currentTime;

        }



        pos = mtrkPos + 1;

    }



    qDebug() << "[NobFileHandler] Total ticks:" << maxTicks;

    return maxTicks;

}



unsigned long NobFileHandler::calculateFirstNoteOnTick(const QString& filePath)

{

    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        qDebug() << "[NobFileHandler] Failed to open NOB file:" << filePath;

        return 0;

    }



    QByteArray data = file.readAll();

    file.close();



    if (data.size() < MIDI_OFFSET) {

        qDebug() << "[NobFileHandler] File too small to contain MIDI data";

        return 0;

    }



    // Extract MIDI data (skip 69-byte header)

    QByteArray midiData = data.mid(MIDI_OFFSET);



    unsigned long firstNoteOn = 0;

    bool foundFirstNote = false;

    int pos = 0;



    // Find all MTrk chunks

    while (pos < midiData.size()) {

        int mtrkPos = midiData.indexOf("MTrk", pos);

        if (mtrkPos == -1) {

            break;

        }



        pos = mtrkPos + 4;

        if (pos + 4 > midiData.size()) {

            break;

        }



        // Read track length (big-endian)

        unsigned long trackLen = (static_cast<unsigned char>(midiData[pos]) << 24) |

                                 (static_cast<unsigned char>(midiData[pos + 1]) << 16) |

                                 (static_cast<unsigned char>(midiData[pos + 2]) << 8) |

                                 static_cast<unsigned char>(midiData[pos + 3]);

        pos += 4;



        int trackEnd = pos + trackLen;

        unsigned long currentTime = 0;

        unsigned char runningStatus = 0;



        // Parse track events

        int offset = pos;

        while (offset < trackEnd && offset < midiData.size()) {

            // Read variable-length delta time

            unsigned long deltaTime = 0;

            while (offset < trackEnd) {

                unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                deltaTime = (deltaTime << 7) | (byte & 0x7F);

                if (!(byte & 0x80)) {

                    break;

                }

            }



            currentTime += deltaTime;



            if (offset >= trackEnd) {

                break;

            }



            // Read event type

            unsigned char eventByte = static_cast<unsigned char>(midiData[offset]);

            unsigned char status;



            if (eventByte & 0x80) {

                // New status byte

                status = eventByte;

                runningStatus = status;

                offset++;

            } else {

                // Running status

                status = runningStatus;

            }



            unsigned char eventType = status & 0xF0;



            // Note On event (0x90)

            if (eventType == 0x90) {

                if (offset + 1 < trackEnd) {

                    unsigned char velocity = static_cast<unsigned char>(midiData[offset + 1]);

                    if (velocity > 0) {

                        // Found first actual note on

                        if (!foundFirstNote) {

                            firstNoteOn = currentTime;

                            foundFirstNote = true;

                            qDebug() << "[NobFileHandler] First note on at tick:" << firstNoteOn;

                            // Don't return here - keep parsing to ensure running status is maintained

                        }

                    }

                    offset += 2;

                } else {

                    break;

                }

            }

            // Note Off event (0x80)

            else if (eventType == 0x80) {

                if (offset + 1 < trackEnd) {

                    offset += 2;

                } else {

                    break;

                }

            }

            // Meta event (0xFF)

            else if (status == 0xFF) {

                if (offset >= trackEnd) {

                    break;

                }

                offset++; // Skip meta type

                // Read meta length

                unsigned long metaLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    metaLen = (metaLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += metaLen;

            }

            // Program Change (0xC0)

            else if (eventType == 0xC0) {

                if (offset < trackEnd) {

                    offset += 1;

                }

            }

            // Control Change (0xB0) or Pitch Bend (0xE0)

            else if (eventType == 0xB0 || eventType == 0xE0) {

                if (offset + 1 < trackEnd) {

                    offset += 2;

                } else {

                    break;

                }

            }

            // After Touch (0xA0) or Channel Pressure (0xD0)

            else if (eventType == 0xA0 || eventType == 0xD0) {

                if (offset < trackEnd) {

                    offset += 1;

                }

            }

            // SysEx events (0xF0, 0xF7)

            else if (status == 0xF0 || status == 0xF7) {

                unsigned long sysexLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    sysexLen = (sysexLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += sysexLen;

            }

        }



        pos = trackEnd;



        // If we found the first note, we can stop searching

        if (foundFirstNote) {

            break;

        }

    }



    return firstNoteOn;

}



QList<unsigned long> NobFileHandler::extractLyricMarkerTicks(const QString& filePath)

{

    QList<unsigned long> markerTicks;



    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly)) {

        qDebug() << "[NobFileHandler] Failed to open NOB file:" << filePath;

        return markerTicks;

    }



    QByteArray data = file.readAll();

    file.close();



    if (data.size() < MIDI_OFFSET) {

        qDebug() << "[NobFileHandler] File too small";

        return markerTicks;

    }



    // Extract lyrics to get count

    QStringList lyrics = extractLyrics(filePath);

    if (lyrics.isEmpty()) {

        qDebug() << "[NobFileHandler] No lyrics found";

        return markerTicks;

    }



    // Extract MIDI data

    QByteArray midiData = data.mid(MIDI_OFFSET);



    int pos = 0;

    if (midiData.size() < 14 || memcmp(midiData.constData(), "MThd", 4) != 0) {

        return markerTicks;

    }



    pos += 4;

    pos += 4; // Skip header length



    pos += 4; // Skip format and track count

    pos += 2; // Skip TPQN



    QList<unsigned long> channel10Notes;



    // Find all channel 10 (index 10) note on events

    while (pos < midiData.size()) {

        int mtrkPos = midiData.indexOf("MTrk", pos);

        if (mtrkPos == -1) {

            break;

        }



        pos = mtrkPos + 4;

        if (pos + 4 > midiData.size()) {

            break;

        }



        unsigned long trackLen = (static_cast<unsigned char>(midiData[pos]) << 24) |

                                 (static_cast<unsigned char>(midiData[pos + 1]) << 16) |

                                 (static_cast<unsigned char>(midiData[pos + 2]) << 8) |

                                 static_cast<unsigned char>(midiData[pos + 3]);

        pos += 4;



        int trackEnd = pos + trackLen;

        unsigned long currentTime = 0;

        unsigned char runningStatus = 0;



        int offset = pos;

        while (offset < trackEnd && offset < midiData.size()) {

            // Read delta time

            unsigned long deltaTime = 0;

            while (offset < trackEnd) {

                unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                deltaTime = (deltaTime << 7) | (byte & 0x7F);

                if (!(byte & 0x80)) {

                    break;

                }

            }



            currentTime += deltaTime;



            if (offset >= trackEnd) {

                break;

            }



            unsigned char eventByte = static_cast<unsigned char>(midiData[offset]);

            unsigned char status;



            if (eventByte & 0x80) {

                status = eventByte;

                runningStatus = status;

                offset++;

            } else {

                status = runningStatus;

            }



            unsigned char eventType = status & 0xF0;

            unsigned char channel = status & 0x0F;



            // Channel 11 = index 10

            if (eventType == 0x90 && channel == 10) {

                if (offset + 1 < trackEnd) {

                    unsigned char velocity = static_cast<unsigned char>(midiData[offset + 1]);

                    if (velocity > 0) {

                        channel10Notes.append(currentTime);

                    }

                    offset += 2;

                } else {

                    break;

                }

            }

            else if (eventType == 0x90 || eventType == 0x80) {

                if (offset + 1 < trackEnd) {

                    offset += 2;

                } else {

                    break;

                }

            }

            else if (status == 0xFF) {

                if (offset >= trackEnd) {

                    break;

                }

                offset++;

                unsigned long metaLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    metaLen = (metaLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += metaLen;

            }

            else if (eventType == 0xC0) {

                if (offset < trackEnd) {

                    offset += 1;

                }

            }

            else if (eventType == 0xB0 || eventType == 0xE0) {

                if (offset + 1 < trackEnd) {

                    offset += 2;

                } else {

                    break;

                }

            }

            else if (eventType == 0xA0 || eventType == 0xD0) {

                if (offset < trackEnd) {

                    offset += 1;

                }

            }

            else if (status == 0xF0 || status == 0xF7) {

                unsigned long sysexLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    sysexLen = (sysexLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += sysexLen;

            }

        }



        pos = trackEnd;

    }



    // Check if we have enough markers

    if (channel10Notes.isEmpty()) {

        qDebug() << "[NobFileHandler] No channel 10 markers found";

        return markerTicks;

    }



    // Check ratio: need at least 3 markers per lyric line

    double ratio = static_cast<double>(channel10Notes.size()) / lyrics.size();

    if (ratio < 3.0) {

        qDebug() << "[NobFileHandler] Not enough markers (" << channel10Notes.size() << " / " << lyrics.size() << " = " << ratio << ")";

        return markerTicks;

    }



    // Skip first marker (intro/preparation signal)

    // Divide remaining markers into groups for each lyric line

    QList<unsigned long> remainingMarkers = channel10Notes.mid(1);

    int markersPerLine = remainingMarkers.size() / lyrics.size();



    qDebug() << "[NobFileHandler] Found" << channel10Notes.size() << "channel 10 markers for" << lyrics.size() << "lyrics";

    qDebug() << "[NobFileHandler] Markers per line:" << markersPerLine << "(first marker skipped)";



    // Extract first marker of each group

    for (int i = 0; i < lyrics.size(); i++) {

        int markerIndex = i * markersPerLine;

        if (markerIndex < remainingMarkers.size()) {

            markerTicks.append(remainingMarkers[markerIndex]);

        }

    }



    return markerTicks;

}



QString NobFileHandler::decodeJohab(const QByteArray& data)

{

    if (data.isEmpty()) {

        return QString();

    }



    // NULL 諛붿씠???쒓굅

    QByteArray cleaned;

    for (int i = 0; i < data.size(); ++i) {

        if (data[i] != 0x00) {

            cleaned.append(data[i]);

        }

    }



    if (cleaned.isEmpty()) {

        return QString();

    }



    // Windows API瑜??ъ슜?섏뿬 議고빀???쒓?(Johab, CP1361) ?붿퐫??

    int wideLen = MultiByteToWideChar(1361, 0, cleaned.constData(), cleaned.size(), NULL, 0);

    if (wideLen == 0) {

        // ?ㅽ뙣 ??ASCII濡?泥섎━

        return QString::fromLatin1(cleaned);

    }



    wchar_t* wideBuf = new wchar_t[wideLen + 1];

    MultiByteToWideChar(1361, 0, cleaned.constData(), cleaned.size(), wideBuf, wideLen);

    wideBuf[wideLen] = 0;



    QString result = QString::fromWCharArray(wideBuf);

    delete[] wideBuf;



    return result;

}



int NobFileHandler::detectMarkerChannel(const QString& filePath)

{

    // 媛??異붿텧

    QStringList lyrics = extractLyrics(filePath);

    if (lyrics.isEmpty()) {

        qDebug() << "[NobFileHandler] No lyrics for marker detection";

        return -1;

    }



    // 媛???⑥쐞 ??怨꾩궛 (怨듬갚 ?쒖쇅, ?섏씠???ы븿) - 諛뺤옄 湲곕컲 ?깊겕

    // Count lyric units for beat sync (skip spaces and hyphens)

    int totalUnits = 0;

    for (const QString& line : lyrics) {

        for (const QChar& ch : line) {

            if (ch != ' ' && ch != '-' && ch != '@') {  // 怨듬갚怨??섏씠?덉? 移댁슫?몄뿉???쒖쇅

                totalUnits++;

            }

        }

    }



    if (totalUnits == 0) {

        qDebug() << "[NobFileHandler] No lyric units";

        return -1;

    }



    // MIDI ?곗씠??異붿텧

    QByteArray midiData = extractMidiData(filePath);

    if (midiData.isEmpty()) {

        qDebug() << "[NobFileHandler] No MIDI data";

        return -1;

    }



    // 媛?梨꾨꼸??Note On ?대깽????怨꾩궛

    QMap<int, int> channelEventCounts;

    QMap<int, unsigned long> channelFirstTick; // 媛?梨꾨꼸??泥?留덉빱 ?쒖옉 ?쒓컙



    int pos = 0;

    if (midiData.size() < 14 || memcmp(midiData.constData(), "MThd", 4) != 0) {

        return -1;

    }



    pos += 4;

    pos += 4; // Skip header length

    pos += 4; // Skip format and track count

    pos += 2; // Skip TPQN



    // 紐⑤뱺 ?몃옓 ?쒗쉶

    while (pos < midiData.size()) {

        int mtrkPos = midiData.indexOf("MTrk", pos);

        if (mtrkPos == -1) {

            break;

        }



        pos = mtrkPos + 4;

        if (pos + 4 > midiData.size()) {

            break;

        }



        unsigned long trackLen = (static_cast<unsigned char>(midiData[pos]) << 24) |

                                 (static_cast<unsigned char>(midiData[pos + 1]) << 16) |

                                 (static_cast<unsigned char>(midiData[pos + 2]) << 8) |

                                 static_cast<unsigned char>(midiData[pos + 3]);

        pos += 4;



        int trackEnd = pos + trackLen;

        unsigned long currentTime = 0;

        unsigned char runningStatus = 0;



        int offset = pos;

        while (offset < trackEnd && offset < midiData.size()) {

            // Read delta time

            unsigned long deltaTime = 0;

            while (offset < trackEnd) {

                unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                deltaTime = (deltaTime << 7) | (byte & 0x7F);

                if (!(byte & 0x80)) {

                    break;

                }

            }



            if (offset >= trackEnd) {

                break;

            }

            currentTime += deltaTime;





            unsigned char eventByte = static_cast<unsigned char>(midiData[offset]);

            unsigned char status;



            if (eventByte & 0x80) {

                status = eventByte;

                runningStatus = status;

                offset++;

            } else {

                status = runningStatus;

            }



            unsigned char eventType = status & 0xF0;

            unsigned char channel = status & 0x0F;



            // Note On ?대깽??移댁슫??

            if (eventType == 0x90) {

                if (offset + 1 < trackEnd) {

                    unsigned char velocity = static_cast<unsigned char>(midiData[offset + 1]);

                    if (velocity > 0) {

                        if (channelEventCounts.contains(channel)) {

                            channelEventCounts[channel]++;

                        } else {

                            channelEventCounts[channel] = 1;

                            channelFirstTick[channel] = currentTime; // 泥?留덉빱 ?쒓컙 ???

                        }

                    }

                    offset += 2;

                } else {

                    break;

                }

            }

            else if (eventType == 0x80 || eventType == 0xA0 || eventType == 0xB0 || eventType == 0xE0) {

                offset += 2;

            }

            else if (eventType == 0xC0 || eventType == 0xD0) {

                offset += 1;

            }

            else if (status == 0xFF) {

                if (offset >= trackEnd) {

                    break;

                }

                offset++;

                unsigned long metaLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    metaLen = (metaLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += metaLen;

            }

            else if (status == 0xF0 || status == 0xF7) {

                unsigned long sysexLen = 0;

                while (offset < trackEnd) {

                    unsigned char byte = static_cast<unsigned char>(midiData[offset++]);

                    sysexLen = (sysexLen << 7) | (byte & 0x7F);

                    if (!(byte & 0x80)) {

                        break;

                    }

                }

                offset += sysexLen;

            }

        }



        pos = trackEnd;

    }



    // 梨꾨꼸蹂??먯닔 怨꾩궛: 鍮꾩쑉 + ?쒖옉 吏???쒓컙 議고빀

    int bestChannel = -1;

    double bestScore = -1.0;



    for (auto it = channelEventCounts.constBegin(); it != channelEventCounts.constEnd(); ++it) {

        int channel = it.key();

        int eventCount = it.value();

        double ratio = static_cast<double>(eventCount) / static_cast<double>(totalUnits);



        if (ratio <= 0.0) {

            continue;

        }



        bool ratioCoreRange = (ratio >= 0.5 && ratio <= 1.5);

        double ratioScore = 0.0;



        if (ratioCoreRange) {

            ratioScore = 1.0 - std::abs(ratio - 1.0);

            if (ratioScore < 0.0) {

                ratioScore = 0.0;

            }

        } else {

            double distance = std::abs(std::log(ratio));

            ratioScore = 1.0 / (1.0 + distance);



            if (ratio < 0.25 || ratio > 4.0) {

                ratioScore *= 0.6;

            }

        }



        double delayBonus = 0.0;

        if (channelFirstTick.contains(channel)) {

            unsigned long firstTick = channelFirstTick[channel];



            if (firstTick >= 3000) {
                delayBonus = 0.8;
            } else if (firstTick >= 2000) {
                delayBonus = 0.65;
            } else if (firstTick >= 1200) {
                delayBonus = 0.5;
            } else if (firstTick >= 900) {
                delayBonus = 0.35;
            } else if (firstTick >= 700) {
                delayBonus = 0.2;
            }
        }



        double totalScore = ratioScore + delayBonus;

        if (!ratioCoreRange && delayBonus <= 0.0) {
            totalScore *= 0.75;
        }


        if (totalScore > bestScore) {

            bestScore = totalScore;

            bestChannel = channel;

        }

    }



    if (bestChannel == -1) {

        qDebug() << "[NobFileHandler] No suitable marker channel found";

        return -1;

    }



    qDebug() << "[NobFileHandler] Detected marker channel:" << (bestChannel + 1)

             << "with" << channelEventCounts[bestChannel] << "events,"

             << totalUnits << "lyric units, ratio="

             << (static_cast<double>(channelEventCounts[bestChannel]) / totalUnits);



    return bestChannel + 1; // Return 1-based channel number

}


