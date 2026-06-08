#include "okafilehandler.h"
#include <QFile>
#include <QByteArray>
#include <QDebug>
#include <windows.h>

static const char* OKA_SIGNATURE = "Oksori Music File";

bool OkaFileHandler::isOkaFile(const QString& filePath)
{
    QString lower = filePath.toLower();
    if (!(lower.endsWith(".oka") || lower.endsWith(".okm")))
        return false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray sig = file.read(17);
    file.close();
    return sig == QByteArray(OKA_SIGNATURE);
}

QString OkaFileHandler::extractTitle(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray data = file.read(OKA_MUSIC_OFFSET);   // header region only
    file.close();
    if (data.size() <= OKA_TITLE_OFFSET) return {};

    // Title is plain Johab at 0x27, NUL-terminated.
    int end = OKA_TITLE_OFFSET;
    while (end < data.size() && (unsigned char)data[end] != 0x00) ++end;
    if (end <= OKA_TITLE_OFFSET) return {};
    return decodeJohab(data.mid(OKA_TITLE_OFFSET, end - OKA_TITLE_OFFSET));
}

QString OkaFileHandler::extractTitle(const QByteArray& fileData)
{
    if (fileData.size() <= OKA_TITLE_OFFSET) return {};

    // Title is plain Johab at 0x27, NUL-terminated.
    int end = OKA_TITLE_OFFSET;
    while (end < fileData.size() && (unsigned char)fileData[end] != 0x00 && end < OKA_MUSIC_OFFSET) ++end;
    if (end <= OKA_TITLE_OFFSET) return {};
    return decodeJohab(fileData.mid(OKA_TITLE_OFFSET, end - OKA_TITLE_OFFSET));
}

QByteArray OkaFileHandler::extractMidiData(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray raw = file.readAll();
    file.close();
    const int fileSize = raw.size();
    if (fileSize < 0x310) return {};

    // Read MIDI size from header offset 0x1CE (Little-Endian)
    unsigned int midiSize = 0;
    memcpy(&midiSize, raw.constData() + 0x1CE, 4);

    if (0x310 + midiSize > (unsigned int)fileSize) {
        midiSize = fileSize - 0x310;
    }

    QByteArray dec(midiSize, Qt::Uninitialized);
    const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.constData() + 0x310);
    for (unsigned int i = 0; i < midiSize; ++i) {
        dec[i] = (char)(src[i] ^ OKA_XOR_KEY);
    }

    qDebug() << "[OkaFileHandler] Extracted MIDI from" << filePath
             << "midiSize=" << midiSize;

    return dec;
}

QStringList OkaFileHandler::extractInstrumentNames(const QString& filePath)
{
    QStringList slotNames;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return slotNames;
    QByteArray raw = file.readAll();
    file.close();
    const int fileSize = raw.size();
    if (fileSize < 0x310) return slotNames;

    // Read sizes from header (Little-Endian)
    unsigned int midiSize = 0;
    unsigned int lyricSize = 0;
    unsigned short textSize = 0;
    unsigned short val1b4 = 0;
    unsigned short instSize = 0;

    memcpy(&midiSize, raw.constData() + 0x1CE, 4);
    memcpy(&lyricSize, raw.constData() + 0x1AE, 4);
    memcpy(&textSize, raw.constData() + 0x1B2, 2);
    memcpy(&val1b4, raw.constData() + 0x1B4, 2);
    memcpy(&instSize, raw.constData() + 0x1D2, 2);

    if (instSize == 0) return slotNames;

    unsigned int instStart = 0x310 + midiSize + lyricSize + textSize + val1b4;
    if (instStart + instSize > (unsigned int)fileSize) {
        qWarning() << "[OkaFileHandler] extractInstrumentNames: out of boundary in" << filePath;
        return slotNames;
    }

    QByteArray instData(instSize, Qt::Uninitialized);
    const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.constData() + instStart);
    for (int i = 0; i < instSize; ++i) {
        instData[i] = (char)(src[i] ^ OKA_XOR_KEY);
    }

    int recCount = instSize / OKA_INST_RECORD_LEN;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(instData.constData());

    for (int i = 0; i < recCount; ++i) {
        int off = i * OKA_INST_RECORD_LEN;
        int len = 0;
        while (len < OKA_INST_NAME_LEN && d[off + len] != 0x00) ++len;
        slotNames << QString::fromLocal8Bit(reinterpret_cast<const char*>(d + off), len);
    }

    qDebug() << "[OkaFileHandler] Instrument table from" << filePath
             << "slots=" << slotNames.size() << slotNames;

    return slotNames;
}

QStringList OkaFileHandler::extractLyrics(const QString& filePath)
{
    QStringList lines;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return lines;
    QByteArray raw = file.readAll();
    file.close();
    const int fileSize = raw.size();
    if (fileSize < 0x310) return lines;

    // Read sizes from header (Little-Endian)
    unsigned int midiSize = 0;
    unsigned int lyricSize = 0;
    unsigned short textSize = 0;

    memcpy(&midiSize, raw.constData() + 0x1CE, 4);
    memcpy(&lyricSize, raw.constData() + 0x1AE, 4);
    memcpy(&textSize, raw.constData() + 0x1B2, 2);

    if (textSize == 0) return lines;

    unsigned int textStart = 0x310 + midiSize + lyricSize;
    if (textStart + textSize > (unsigned int)fileSize) {
        qWarning() << "[OkaFileHandler] extractLyrics: out of boundary in" << filePath;
        return lines;
    }

    QByteArray textData(textSize, Qt::Uninitialized);
    const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.constData() + textStart);
    for (int i = 0; i < textSize; ++i) {
        textData[i] = (char)(src[i] ^ OKA_XOR_KEY);
    }

    // Split by CR (0x0D) to handle lines
    QByteArray currentLine;
    for (int i = 0; i < textData.size(); ++i) {
        char b = textData[i];
        if (b == 0x0D) {
            QString decoded = decodeJohab(currentLine).trimmed();
            if (!decoded.isEmpty()) {
                lines.append(decoded);
            }
            currentLine.clear();
        } else if (b == 0x0A) {
            // Skip LF
        } else {
            currentLine.append(b);
        }
    }
    if (!currentLine.isEmpty()) {
        QString decoded = decodeJohab(currentLine).trimmed();
        if (!decoded.isEmpty()) {
            lines.append(decoded);
        }
    }

    qDebug() << "[OkaFileHandler] Extracted" << lines.size() << "lyrics lines from" << filePath;
    return lines;
}

QString OkaFileHandler::decodeJohab(const QByteArray& data)
{
    static const int JUNG_MAP[32] = {
        -1, -1, -1,  0,  1,  2,  3,  4, -1, -1,  5,  6,  7,  8,  9, 10,
        -1, -1, 11, 12, 13, 14, 15, 16, -1, -1, 17, 18, 19, 20, -1, -1
    };

    static const int JONG_MAP[32] = {
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, -1, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, -1, -1
    };

    QString result;
    result.reserve(data.size() / 2);
    int size = data.size();
    for (int i = 0; i < size; ) {
        unsigned char c1 = (unsigned char)data[i];
        if (c1 >= 0x80) { // Johab hangul (2-byte)
            if (i + 1 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                unsigned short code = (c1 << 8) | c2;
                
                // Johab hangul range check: 0x8441 ~ 0xD3FE
                if (code >= 0x8400 && code <= 0xD3FF) {
                    int cho = (code >> 10) & 0x1F;
                    int jung = (code >> 5) & 0x1F;
                    int jong = code & 0x1F;
                    
                    int choIdx = (cho >= 2 && cho <= 20) ? cho - 2 : -1;
                    int jungIdx = (jung >= 0 && jung < 32) ? JUNG_MAP[jung] : -1;
                    int jongIdx = (jong >= 0 && jong < 32) ? JONG_MAP[jong] : -1;
                    
                    if (choIdx >= 0 && choIdx < 19 && jungIdx >= 0 && jungIdx < 21 && jongIdx >= 0 && jongIdx < 28) {
                        wchar_t uni = 0xAC00 + (choIdx * 21 * 28) + (jungIdx * 28) + jongIdx;
                        result.append(QChar(uni));
                        i += 2;
                        continue;
                    }
                }
                
                // Invalid Johab hangul sequences are timing bytes/control marks. Do not output anything.
                i += 2;
            } else {
                i++;
            }
        } else {
            if (c1 != 0) {
                // Filter out timing/sync control chars (isolated alphabetic characters)
                bool isAlpha = ((c1 >= 'a' && c1 <= 'z') || (c1 >= 'A' && c1 <= 'Z'));
                bool isDigit = (c1 >= '0' && c1 <= '9');
                if (isAlpha) {
                    bool prevIsAlphaNum = false;
                    if (!result.isEmpty()) {
                        QChar prev = result.at(result.size() - 1);
                        ushort prevVal = prev.unicode();
                        prevIsAlphaNum = ((prevVal >= 'a' && prevVal <= 'z') || 
                                          (prevVal >= 'A' && prevVal <= 'Z') || 
                                          (prevVal >= '0' && prevVal <= '9'));
                    }
                    bool nextIsAlphaNum = false;
                    if (i + 1 < size) {
                        unsigned char next = (unsigned char)data[i + 1];
                        nextIsAlphaNum = ((next >= 'a' && next <= 'z') || 
                                          (next >= 'A' && next <= 'Z') || 
                                          (next >= '0' && next <= '9'));
                    }
                    if (!prevIsAlphaNum && !nextIsAlphaNum) {
                        i++;
                        continue; // Skip isolated alphabetic character (control/timing mark)
                    }
                }
                
                // Allow valid alphanumeric and basic lyric punctuation
                if (isAlpha || isDigit || c1 == ' ' || c1 == '-' || c1 == '?' || c1 == '!' || 
                    c1 == '.' || c1 == ',' || c1 == '(' || c1 == ')' || c1 == '[' || c1 == ']' ||
                    c1 == '/' || c1 == '*' || c1 == ':' || c1 == ';' || c1 == '\'' || c1 == '"' || c1 == '@') {
                    result.append(QChar(c1));
                }
            }
            i++;
        }
    }
    return result.trimmed();
}

QList<unsigned long> OkaFileHandler::extractLyricMarkerTicks(const QString& filePath)
{
    QList<unsigned long> mappedTicks;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return mappedTicks;
    QByteArray raw = file.readAll();
    file.close();
    const int fileSize = raw.size();
    if (fileSize < 0x310) return mappedTicks;

    unsigned int midiSize = 0;
    unsigned int lyricSize = 0;
    unsigned short textSize = 0;

    memcpy(&midiSize, raw.constData() + 0x1CE, 4);
    memcpy(&lyricSize, raw.constData() + 0x1AE, 4);
    memcpy(&textSize, raw.constData() + 0x1B2, 2);

    if (lyricSize == 0 || textSize == 0) return mappedTicks;

    unsigned int textStart = 0x310 + midiSize + lyricSize;
    if (textStart + textSize > (unsigned int)fileSize) return mappedTicks;

    QByteArray textData(textSize, Qt::Uninitialized);
    const unsigned char* textSrc = reinterpret_cast<const unsigned char*>(raw.constData() + textStart);
    for (int i = 0; i < textSize; ++i) {
        textData[i] = (char)(textSrc[i] ^ OKA_XOR_KEY);
    }

    unsigned int syncStart = 0x310 + midiSize;
    if (syncStart + lyricSize > (unsigned int)fileSize) return mappedTicks;

    QByteArray syncData(lyricSize, Qt::Uninitialized);
    const unsigned char* syncSrc = reinterpret_cast<const unsigned char*>(raw.constData() + syncStart);
    for (unsigned int i = 0; i < lyricSize; ++i) {
        syncData[i] = (char)(syncSrc[i] ^ OKA_XOR_KEY);
    }

    // 1. Map textData bytes to syllable indexes
    QVector<int> byteToSyllable(textSize, -1);
    int syllableCounter = 0;

    QByteArray currentLine;
    int lineStartOffset = 0;

    for (int idx_b = 0; idx_b < textData.size(); ++idx_b) {
        char b = textData[idx_b];
        if (b == 0x0D) {
            QList<int> mappings;
            QString decoded = decodeJohabWithMapping(currentLine, lineStartOffset, mappings);
            if (!decoded.isEmpty()) {
                for (int ch_idx = 0; ch_idx < decoded.size(); ++ch_idx) {
                    QChar ch = decoded[ch_idx];
                    if (ch != ' ' && ch != '-' && ch != '@') {
                        int byteOffset = (ch_idx < mappings.size()) ? mappings[ch_idx] : -1;
                        if (byteOffset >= 0 && byteOffset < textSize) {
                            byteToSyllable[byteOffset] = syllableCounter;
                            if (byteOffset + 1 < textSize) {
                                byteToSyllable[byteOffset + 1] = syllableCounter;
                            }
                        }
                        syllableCounter++;
                    }
                }
            }
            currentLine.clear();
            lineStartOffset = idx_b + 1;
        } else if (b == 0x0A) {
            lineStartOffset = idx_b + 1;
        } else {
            currentLine.append(b);
        }
    }

    if (!currentLine.isEmpty()) {
        QList<int> mappings;
        QString decoded = decodeJohabWithMapping(currentLine, lineStartOffset, mappings);
        if (!decoded.isEmpty()) {
            for (int ch_idx = 0; ch_idx < decoded.size(); ++ch_idx) {
                QChar ch = decoded[ch_idx];
                if (ch != ' ' && ch != '-' && ch != '@') {
                    int byteOffset = (ch_idx < mappings.size()) ? mappings[ch_idx] : -1;
                    if (byteOffset >= 0 && byteOffset < textSize) {
                        byteToSyllable[byteOffset] = syllableCounter;
                        if (byteOffset + 1 < textSize) {
                            byteToSyllable[byteOffset + 1] = syllableCounter;
                        }
                    }
                    syllableCounter++;
                }
            }
        }
    }

    int totalUnits = syllableCounter;
    if (totalUnits <= 0) return mappedTicks;

    mappedTicks.fill(0, totalUnits);

    const unsigned char* syncPtr = reinterpret_cast<const unsigned char*>(syncData.constData());
    for (int i = 0; i + 5 <= syncData.size(); i += 5) {
        unsigned short tick = 0;
        unsigned short offset = 0;
        memcpy(&tick, syncPtr + i, 2);
        memcpy(&offset, syncPtr + i + 2, 2);
        
        if (offset < textSize) {
            int syllableIdx = byteToSyllable[offset];
            if (syllableIdx >= 0 && syllableIdx < totalUnits) {
                mappedTicks[syllableIdx] = tick;
            }
        }
    }

    // Interpolate missing ticks (0s)
    for (int idx = 0; idx < totalUnits; ++idx) {
        if (mappedTicks[idx] == 0) {
            if (idx > 0) {
                mappedTicks[idx] = mappedTicks[idx - 1];
            } else {
                unsigned long firstVal = 0;
                for (int j = idx + 1; j < totalUnits; ++j) {
                    if (mappedTicks[j] > 0) {
                        firstVal = mappedTicks[j];
                        break;
                    }
                }
                mappedTicks[idx] = firstVal;
            }
        }
    }

    return mappedTicks;
}

int OkaFileHandler::getMidiTicksPerQuarter(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    QByteArray raw = file.read(OKA_MUSIC_OFFSET + 14);
    file.close();
    if (raw.size() < OKA_MUSIC_OFFSET + 14) return 0;

    // MIDI region (XOR'd) starts at 0x310 with "MThd"; division is the last
    // 2 bytes of the 14-byte MThd header (big-endian).
    unsigned char d[14];
    const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.constData() + OKA_MUSIC_OFFSET);
    for (int i = 0; i < 14; ++i) d[i] = (unsigned char)(src[i] ^ OKA_XOR_KEY);
    if (d[0] != 'M' || d[1] != 'T' || d[2] != 'h' || d[3] != 'd') return 0;
    int div = (d[12] << 8) | d[13];
    return (div > 0 && div < 32768) ? div : 0;
}

QString OkaFileHandler::decodeJohabWithMapping(const QByteArray& data, int startOffset, QList<int>& charToByteOffset)
{
    static const int JUNG_MAP[32] = {
        -1, -1, -1,  0,  1,  2,  3,  4, -1, -1,  5,  6,  7,  8,  9, 10,
        -1, -1, 11, 12, 13, 14, 15, 16, -1, -1, 17, 18, 19, 20, -1, -1
    };

    static const int JONG_MAP[32] = {
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, -1, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, -1, -1
    };

    QString result;
    result.reserve(data.size() / 2);
    charToByteOffset.clear();
    charToByteOffset.reserve(data.size() / 2);
    int size = data.size();
    for (int i = 0; i < size; ) {
        unsigned char c1 = (unsigned char)data[i];
        if (c1 >= 0x80) { // Johab hangul (2-byte)
            if (i + 1 < size) {
                unsigned char c2 = (unsigned char)data[i + 1];
                unsigned short code = (c1 << 8) | c2;
                
                if (code >= 0x8400 && code <= 0xD3FF) {
                    int cho = (code >> 10) & 0x1F;
                    int jung = (code >> 5) & 0x1F;
                    int jong = code & 0x1F;
                    
                    int choIdx = (cho >= 2 && cho <= 20) ? cho - 2 : -1;
                    int jungIdx = (jung >= 0 && jung < 32) ? JUNG_MAP[jung] : -1;
                    int jongIdx = (jong >= 0 && jong < 32) ? JONG_MAP[jong] : -1;
                    
                    if (choIdx >= 0 && choIdx < 19 && jungIdx >= 0 && jungIdx < 21 && jongIdx >= 0 && jongIdx < 28) {
                        wchar_t uni = 0xAC00 + (choIdx * 21 * 28) + (jungIdx * 28) + jongIdx;
                        result.append(QChar(uni));
                        charToByteOffset.append(startOffset + i);
                        i += 2;
                        continue;
                    }
                }
                i += 2;
            } else {
                i++;
            }
        } else {
            if (c1 != 0) {
                // Filter out timing/sync control chars (isolated alphabetic characters)
                bool isAlpha = ((c1 >= 'a' && c1 <= 'z') || (c1 >= 'A' && c1 <= 'Z'));
                bool isDigit = (c1 >= '0' && c1 <= '9');
                if (isAlpha) {
                    bool prevIsAlphaNum = false;
                    if (!result.isEmpty()) {
                        QChar prev = result.at(result.size() - 1);
                        ushort prevVal = prev.unicode();
                        prevIsAlphaNum = ((prevVal >= 'a' && prevVal <= 'z') || 
                                          (prevVal >= 'A' && prevVal <= 'Z') || 
                                          (prevVal >= '0' && prevVal <= '9'));
                    }
                    bool nextIsAlphaNum = false;
                    if (i + 1 < size) {
                        unsigned char next = (unsigned char)data[i + 1];
                        nextIsAlphaNum = ((next >= 'a' && next <= 'z') || 
                                          (next >= 'A' && next <= 'Z') || 
                                          (next >= '0' && next <= '9'));
                    }
                    if (!prevIsAlphaNum && !nextIsAlphaNum) {
                        i++;
                        continue; // Skip isolated alphabetic character (control/timing mark)
                    }
                }
                
                // Allow valid alphanumeric and basic lyric punctuation
                if (isAlpha || isDigit || c1 == ' ' || c1 == '-' || c1 == '?' || c1 == '!' || 
                    c1 == '.' || c1 == ',' || c1 == '(' || c1 == ')' || c1 == '[' || c1 == ']' ||
                    c1 == '/' || c1 == '*' || c1 == ':' || c1 == ';' || c1 == '\'' || c1 == '"' || c1 == '@') {
                    result.append(QChar(c1));
                    charToByteOffset.append(startOffset + i);
                }
            }
            i++;
        }
    }

    // trimmed() 보정
    int leadingSpaces = 0;
    while (leadingSpaces < result.size() && result[leadingSpaces].isSpace()) {
        leadingSpaces++;
    }
    int trailingSpaces = 0;
    while (trailingSpaces < result.size() - leadingSpaces && result[result.size() - 1 - trailingSpaces].isSpace()) {
        trailingSpaces++;
    }

    QString trimmedResult = result.mid(leadingSpaces, result.size() - leadingSpaces - trailingSpaces);
    charToByteOffset = charToByteOffset.mid(leadingSpaces, charToByteOffset.size() - leadingSpaces - trailingSpaces);

    return trimmedResult;
}

int OkaFileHandler::extractMelodyChannel(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return 2; // Default to Ch 2
    if (file.size() <= 0x15a) return 2;
    
    file.seek(0x15a);
    char chVal = 0;
    if (file.read(&chVal, 1) == 1) {
        int ch = (unsigned char)chVal + 1; // 0-indexed to 1-indexed
        if (ch >= 1 && ch <= 16) {
            return ch;
        }
    }
    return 2;
}



