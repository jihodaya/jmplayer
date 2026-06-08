#include "gybfilehandler.h"
#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QString>
#include <QDir>
#include <windows.h>
#include <QDebug>
#include <QSet>
#include <algorithm>

bool GybFileHandler::isGybFile(const QString& filePath)
{
    if (!filePath.toLower().endsWith(".gyb")) return false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (file.size() < GYB_HEADER_SIZE) { file.close(); return false; }

    char magic;
    file.read(&magic, 1);
    file.close();
    unsigned char m = (unsigned char)magic;
    return (m == GYB_MAGIC_VALUE_A) || (m == GYB_MAGIC_VALUE_B);
}

QString GybFileHandler::extractTitle(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() < GYB_HEADER_SIZE) return QString();
    file.seek(GYB_TITLE_OFFSET);
    QByteArray titleData = file.read(GYB_TITLE_SIZE);
    file.close();

    // Title is NUL-terminated in the title field
    int nulPos = titleData.indexOf('\0');
    if (nulPos >= 0) titleData.truncate(nulPos);
    return decodeJohab(titleData).trimmed();
}

QString GybFileHandler::extractTitle(const QByteArray& fileData)
{
    if (fileData.size() < GYB_HEADER_SIZE) return QString();
    QByteArray titleData = fileData.mid(GYB_TITLE_OFFSET, GYB_TITLE_SIZE);
    int nulPos = titleData.indexOf('\0');
    if (nulPos >= 0) titleData.truncate(nulPos);
    return decodeJohab(titleData).trimmed();
}

QString GybFileHandler::extractTitleFromLst(const QString& gybFilePath)
{
    QString gybFileName = QFileInfo(gybFilePath).fileName().toLower();
    QFileInfo gybInfo(gybFilePath);
    QString lstPath = gybInfo.absolutePath() + "/GYB.LST";

    QFile lstFile(lstPath);
    if (!lstFile.open(QIODevice::ReadOnly) || lstFile.size() < 8) return QString();

    lstFile.seek(8);
    while (!lstFile.atEnd()) {
        QByteArray record = lstFile.read(38);
        if (record.size() < 38) break;

        QByteArray pathField = record.mid(24, 12);
        QString pathStr = QString::fromLatin1(pathField).trimmed().toLower();
        if (pathStr == gybFileName) {
            QByteArray titleField = record.mid(0, 24);
            lstFile.close();
            return decodeJohab(titleField).trimmed();
        }
        if (!lstFile.atEnd()) lstFile.seek(lstFile.pos() + 2);
    }
    lstFile.close();
    return QString();
}

int GybFileHandler::detectMelodyChannel(const QString& filePath)
{
    QStringList lyrics = extractLyrics(filePath);
    int totalUnits = 0;
    for (const QString& line : lyrics) {
        for (const QChar& ch : line) {
            if (ch != ' ' && ch != '-' && ch != '@') {
                totalUnits++;
            }
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return 1;
    QByteArray data = file.readAll();
    file.close();

    const int fs = data.size();
    if (fs < 0x40) return 1;

    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    int magic = d[0];
    int pos = (magic == 0x03) ? 0x40 : 0x4F;
    if (pos + 2 > fs) return 1;

    int gc = d[pos] | (d[pos + 1] << 8); pos += 2;
    if (gc > 1000) gc = 0;
    pos += gc * 4;

    QMap<int, int> channelEventCounts;
    QMap<int, unsigned long> channelFirstTick;

    // Only scan melodic channels 0..5 (6..10 are drums/percussion in GYB)
    for (int ch = 0; ch < 6; ++ch) {
        if (pos + 3 >= fs) break;
        if (d[pos] != ch) break;
        pos += 1;
        int endTick = d[pos] | (d[pos + 1] << 8); pos += 2;

        int curTick = 0;
        int noteStart = pos;
        int noteOnCount = 0;
        unsigned long firstTick = 0xFFFFFFFF;

        while (curTick < endTick && pos + 1 < fs) {
            unsigned char cmd = d[pos];
            unsigned char dur = d[pos + 1];
            if (cmd > 0 && cmd < 0x79) { // Note On
                noteOnCount++;
                if (firstTick == 0xFFFFFFFF) {
                    firstTick = curTick;
                }
            }
            curTick += dur;
            pos += 2;
        }

        channelEventCounts[ch] = noteOnCount;
        if (firstTick != 0xFFFFFFFF) {
            channelFirstTick[ch] = firstTick;
        }

        // Jump to next channel header
        pos = noteStart;
        curTick = 0;
        while (curTick < endTick && pos + 1 < fs) {
            curTick += d[pos + 1]; pos += 2;
        }
        if (pos + 1 >= fs) break;
        int pc = d[pos] | (d[pos + 1] << 8); pos += 2 + pc * 4;
        if (pos + 1 >= fs) break;
        int vc = d[pos] | (d[pos + 1] << 8); pos += 2 + vc * 4;
        if (pos + 1 >= fs) break;
        int ptc = d[pos] | (d[pos + 1] << 8); pos += 2 + ptc * 4;
    }

    if (totalUnits == 0) {
        // Fallback to earliest note channel if no lyrics
        int bestChannel = 0;
        unsigned long minTick = 0xFFFFFFFF;
        for (auto it = channelFirstTick.constBegin(); it != channelFirstTick.constEnd(); ++it) {
            if (it.value() < minTick) {
                minTick = it.value();
                bestChannel = it.key();
            }
        }
        return bestChannel + 1;
    }

    int bestChannel = -1;
    double bestScore = -1.0;

    for (auto it = channelEventCounts.constBegin(); it != channelEventCounts.constEnd(); ++it) {
        int ch = it.key();
        int eventCount = it.value();
        double ratio = static_cast<double>(eventCount) / static_cast<double>(totalUnits);

        if (ratio <= 0.0) continue;

        bool ratioCoreRange = (ratio >= 0.5 && ratio <= 1.5);
        double ratioScore = 0.0;

        if (ratioCoreRange) {
            ratioScore = 1.0 - std::abs(ratio - 1.0);
            if (ratioScore < 0.0) ratioScore = 0.0;
        } else {
            double distance = std::abs(std::log(ratio));
            ratioScore = 1.0 / (1.0 + distance);
            if (ratio < 0.25 || ratio > 4.0) ratioScore *= 0.6;
        }

        double delayBonus = 0.0;
        if (channelFirstTick.contains(ch)) {
            unsigned long firstTick = channelFirstTick[ch];
            // In GYB, 16 seconds is roughly 100-110 ticks under 6.67Hz
            if (firstTick >= 300)      delayBonus = 0.8;
            else if (firstTick >= 200) delayBonus = 0.65;
            else if (firstTick >= 120) delayBonus = 0.5;
            else if (firstTick >= 90)  delayBonus = 0.35;
            else if (firstTick >= 50)  delayBonus = 0.2;
        }

        double totalScore = ratioScore + delayBonus;
        if (!ratioCoreRange && delayBonus <= 0.0) {
            totalScore *= 0.75;
        }

        if (totalScore > bestScore) {
            bestScore = totalScore;
            bestChannel = ch;
        }
    }

    if (bestChannel == -1) {
        return 1; // Fallback
    }

    qDebug() << "[GybFileHandler] Auto-detected melody channel:" << bestChannel + 1
             << "with score:" << bestScore;
    return bestChannel + 1;
}

QList<GybFileHandler::ChannelInfo> GybFileHandler::parseChannelTable(const QString& filePath)
{
    // GYB sequence layout (reverse-engineered from GAYOBANG.EXE FUN_28a1_18f0):
    //   payload_start (0x4F for v4, 0x40 for v3):
    //     uint16  global_event_count
    //     [4 bytes × global_event_count]  (tick:uint16, value:uint16) — tempo/master events
    //     // Per-channel block (channels 0..10 in order):
    //     byte    channel_id (must equal expected channel number)
    //     uint16  end_tick
    //     byte[]  note_stream — (cmd, dur) pairs until sum of dur >= end_tick
    //     uint16  prog_change_count
    //     [4 bytes × prog_change_count]   (tick:uint16, value:byte, pad:byte)
    //     uint16  volume_count
    //     [4 bytes × volume_count]
    //     uint16  pitch_count
    //     [4 bytes × pitch_count]
    QList<ChannelInfo> result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return result;
    QByteArray data = file.readAll();
    file.close();

    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE + 2) return result;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());

    // Pick payload offset by magic byte. Magic 0x04 has a 15-byte secondary header.
    int pos = (d[0] == GYB_MAGIC_VALUE_B) ? GYB_PAYLOAD_OFFSET_V3 : GYB_PAYLOAD_OFFSET_V4;
    if (pos + 2 > fileSize) return result;

    // Skip global events
    int globalCount = d[pos] | (d[pos+1] << 8);
    pos += 2;
    if (globalCount > 1000) {
        qDebug() << "[GybFileHandler] suspicious global event count" << globalCount;
        return result;
    }
    pos += globalCount * 4;

    for (int ch = 0; ch < GYB_NUM_CHANNELS; ++ch) {
        if (pos + 3 >= fileSize) break;
        if (d[pos] != ch) {
            qDebug() << "[GybFileHandler] channel id mismatch at 0x" << QString::number(pos, 16)
                     << "expected" << ch << "got" << d[pos];
            break;
        }
        pos += 1;
        int endTick = d[pos] | (d[pos+1] << 8);
        pos += 2;

        int noteStart = pos;
        int total = 0;
        while (total < endTick && pos + 1 < fileSize) {
            int dur = d[pos+1];
            total += dur;
            pos += 2;
        }
        int noteEnd = pos;

        if (pos + 1 >= fileSize) break;
        int pc = d[pos] | (d[pos+1] << 8); pos += 2;
        int pcOff = pos;
        pos += pc * 4;

        if (pos + 1 >= fileSize) break;
        int vc = d[pos] | (d[pos+1] << 8); pos += 2;
        int vcOff = pos;
        pos += vc * 4;

        if (pos + 1 >= fileSize) break;
        int ptc = d[pos] | (d[pos+1] << 8); pos += 2;
        int ptcOff = pos;
        pos += ptc * 4;

        ChannelInfo c;
        c.channelId = ch;
        c.noteStartOffset = noteStart;
        c.noteEndOffset = noteEnd;
        c.endTick = endTick;
        c.programCount = pc;
        c.programOffset = pcOff;
        c.volumeCount = vc;
        c.volumeOffset = vcOff;
        c.pitchCount = ptc;
        c.pitchOffset = ptcOff;
        c.isLyrics = false;
        c.headerOffset = ch;
        c.startOffset = noteStart;
        c.endOffset = noteEnd;
        result.append(c);
    }

    qDebug() << "[GybFileHandler] parsed" << result.size() << "channels (sequential format) from" << filePath;
    for (const ChannelInfo& c : result) {
        qDebug().nospace() << "  ch" << c.channelId << " endTick=" << c.endTick
                           << " notes=0x" << Qt::hex << c.noteStartOffset
                           << "-0x" << c.noteEndOffset
                           << " pc=" << Qt::dec << c.programCount
                           << " vol=" << c.volumeCount
                           << " ptc=" << c.pitchCount;
    }
    return result;
}

QList<GybFileHandler::InstrumentInfo> GybFileHandler::parseInstrumentTable(const QString& filePath)
{
    // The instrument table immediately follows the last channel's pitch-event block.
    // Each entry is 38 bytes: 9-byte ASCII name (NUL-padded) + 29-byte OPL params.
    // Header byte at offset 0x3C (uint16 LE) holds the entry count (default 13).
    QList<InstrumentInfo> result;
    QList<ChannelInfo> channels = parseChannelTable(filePath);
    if (channels.isEmpty()) return result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return result;
    QByteArray data = file.readAll();
    file.close();
    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE + 4) return result;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());

    int count = d[GYB_INST_COUNT_OFFSET] | (d[GYB_INST_COUNT_OFFSET + 1] << 8);
    if (count <= 0 || count > 64) count = 13;  // safety clamp

    // Locate instrument table start: end of channel 10's pitch events.
    const ChannelInfo& lastCh = channels.last();
    int instStart = lastCh.pitchOffset + lastCh.pitchCount * 4;
    if (instStart + count * GYB_INST_RECORD_SIZE > fileSize) {
        qDebug() << "[GybFileHandler] inst table exceeds file: start=0x"
                 << QString::number(instStart, 16) << "count=" << count;
        return result;
    }

    for (int i = 0; i < count; ++i) {
        int off = instStart + i * GYB_INST_RECORD_SIZE;
        InstrumentInfo inst;
        QByteArray nameRaw(reinterpret_cast<const char*>(d + off), GYB_INST_NAME_SIZE);
        int nulPos = nameRaw.indexOf('\0');
        if (nulPos >= 0) nameRaw.truncate(nulPos);
        inst.name = QString::fromLocal8Bit(nameRaw).trimmed();
        inst.oplParams = QByteArray(reinterpret_cast<const char*>(d + off + GYB_INST_NAME_SIZE),
                                    GYB_INST_PARAM_SIZE);
        result.append(inst);
    }
    qDebug() << "[GybFileHandler] parsed" << result.size() << "embedded instruments from 0x"
             << QString::number(instStart, 16);
    return result;
}

QStringList GybFileHandler::extractLyrics(const QString& filePath)
{
    QStringList lyrics;
    QList<ChannelInfo> channels = parseChannelTable(filePath);
    if (channels.isEmpty()) return lyrics;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return lyrics;
    QByteArray data = file.readAll();
    file.close();

    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE) return lyrics;

    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    int count = d[GYB_INST_COUNT_OFFSET] | (d[GYB_INST_COUNT_OFFSET + 1] << 8);
    if (count <= 0 || count > 64) count = 13;

    const ChannelInfo& lastCh = channels.last();
    int instStart = lastCh.pitchOffset + lastCh.pitchCount * 4;
    int lyricStart = instStart + count * GYB_INST_RECORD_SIZE;

    if (lyricStart >= fileSize) {
        return lyrics; // No lyrics found
    }

    QByteArray lyricData = data.mid(lyricStart);

    // English (export) GYB has no Johab (>= 0x80) bytes — lyrics are plain ASCII.
    // decodeJohab()'s isolated-alphabetic "control mark" filtering would wrongly
    // drop single-letter English syllables ("e", "a", "I"), so decode ASCII
    // directly: keep all printable chars + spaces/hyphens, split lines on runs of
    // >= 5 NULs. This keeps the per-character unit count identical to the markers
    // produced by extractLyricSyllableTicks (English path), so highlighting aligns.
    bool hasJohab = false;
    for (char c : lyricData) { if ((unsigned char)c >= 0x80) { hasJohab = true; break; } }
    if (!hasJohab) {
        QByteArray cur;
        int nullRun = 0;
        auto flushLine = [&]() {
            if (cur.isEmpty()) return;
            QString s;
            for (char c : cur) {
                unsigned char b = (unsigned char)c;
                if (b >= 0x20 && b <= 0x7E) s.append(QChar(b));
            }
            s = s.trimmed();
            if (!s.isEmpty()) lyrics.append(s);
            cur.clear();
        };
        for (int i = 0; i < lyricData.size(); ++i) {
            unsigned char b = (unsigned char)lyricData[i];
            if (b == 0) {
                nullRun++;
                if (nullRun >= 5) flushLine();
            } else {
                nullRun = 0;
                cur.append((char)b);
            }
        }
        flushLine();
        return lyrics;
    }

    // --- Korean (Johab) path — UNCHANGED. ---
    for (int i = 0; i < lyricData.size(); i += 75) {
        QByteArray chunk = lyricData.mid(i, 75);
        QString decoded = decodeJohab(chunk).trimmed();
        if (!decoded.isEmpty()) {
            lyrics.append(decoded);
        }
    }

    return lyrics;
}

QList<unsigned long> GybFileHandler::extractLyricMarkerTicks(const QString& filePath, const QStringList& lyrics)
{
    QList<unsigned long> markerTicks;
    if (lyrics.isEmpty()) return markerTicks;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return markerTicks;
    QByteArray data = file.readAll();
    file.close();
    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE) return markerTicks;

    QList<ChannelInfo> channels = parseChannelTable(filePath);
    if (channels.isEmpty()) return markerTicks;

    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    int count = d[GYB_INST_COUNT_OFFSET] | (d[GYB_INST_COUNT_OFFSET + 1] << 8);
    if (count <= 0 || count > 64) count = 13;

    const ChannelInfo& lastCh = channels.last();
    int instStart = lastCh.pitchOffset + lastCh.pitchCount * 4;
    int lyricStart = instStart + count * GYB_INST_RECORD_SIZE;

    if (lyricStart >= fileSize) {
        return markerTicks;
    }

    QByteArray lyricData = data.mid(lyricStart);
    const int totalBytes = lyricData.size();
    if (totalBytes <= 0) return markerTicks;

    // 1. Extract absolute timeline ticks for genuine singing characters from the binary stream
    unsigned long tickCounter = 0;
    QList<unsigned long> originalTicks;

    for (int i = 0; i < totalBytes; ) {
        unsigned char b = (unsigned char)lyricData[i];

        if (b == 0x00) {
            tickCounter++;
            i++;
        } else if (b >= 0x80) {
            // Korean Johab: fallback to char-by-char independent tick (100% isolation)
            if (i + 1 < totalBytes) {
                originalTicks.append(tickCounter);
                tickCounter += 2; // Johab Hangul consumes 2 ticks of lyric-stream time
                i += 2;
            } else {
                break;
            }
        } else {
            // ASCII Area (English, punctuation, spaces, hyphens)
            if (b != 0x20 && b != 0x2D && b >= 0x21 && b <= 0x7E) {
                // Printable English character gets its own unique sequential tick
                originalTicks.append(tickCounter);
            }
            tickCounter++;
            i++;
        }
    }

    // 2. Count total units (syllables) in the displayed lyrics
    int totalUnits = 0;
    for (const QString& line : lyrics) {
        for (const QChar& ch : line) {
            if (ch != ' ' && ch != '-' && ch != '@') {
                totalUnits++;
            }
        }
    }

    if (totalUnits == 0 || originalTicks.isEmpty()) {
        return markerTicks;
    }

    // 3. Map original ticks to target syllables
    QList<unsigned long> finalTicks;
    if (originalTicks.size() == totalUnits) {
        finalTicks = originalTicks;
    } else {
        unsigned long avgDelta = 4;
        if (originalTicks.size() > 1) {
            avgDelta = (originalTicks.last() - originalTicks.first()) / (originalTicks.size() - 1);
            if (avgDelta == 0) avgDelta = 4;
        }

        for (int i = 0; i < totalUnits; ++i) {
            if (i < originalTicks.size()) {
                finalTicks.append(originalTicks[i]);
            } else {
                finalTicks.append(originalTicks.last() + (i - originalTicks.size() + 1) * avgDelta);
            }
        }
    }

    // 4. Calculate tempo parameters
    unsigned long bpm_x100 = 10000;
    unsigned char tb_div = 4;
    if (fileSize >= 0x38) {
        bpm_x100 = d[0x34] | (d[0x35] << 8) | (d[0x36] << 16) | (d[0x37] << 24);
        if (bpm_x100 < 3000 || bpm_x100 > 30000) bpm_x100 = 10000;
        tb_div = d[0x28];
        if (tb_div < 1 || tb_div > 64) tb_div = 4;
    }

    double scaleFactor = (double)bpm_x100 * (double)tb_div / 54620.0;
    double playHz = (double)bpm_x100 * (double)tb_div / 6000.0;
    double delaySec = -1.5928;
    long delayTicks = (long)qRound(playHz * delaySec);

    // 5. Try to find the melody channel for anchoring
    int melodyCh = detectMelodyChannel(filePath) - 1; // 0-based
    QList<unsigned long> vocalTicks;
    bool useFallback = true;

    if (melodyCh >= 0 && melodyCh < 11) {
        // Read note events for the melody channel
        int pos = (d[0] == GYB_MAGIC_VALUE_B) ? GYB_PAYLOAD_OFFSET_V3 : GYB_PAYLOAD_OFFSET_V4;
        if (pos + 2 <= fileSize) {
            int gc = d[pos] | (d[pos + 1] << 8); pos += 2;
            if (gc > 1000) gc = 0;
            pos += gc * 4;

            for (int ch = 0; ch < 11; ++ch) {
                if (pos + 3 >= fileSize) break;
                if (d[pos] != ch) break;
                pos += 1;
                int endTick = d[pos] | (d[pos + 1] << 8); pos += 2;

                int curTick = 0;
                int noteStart = pos;

                while (curTick < endTick && pos + 1 < fileSize) {
                    unsigned char cmd = d[pos];
                    unsigned char dur = d[pos + 1];
                    if (ch == melodyCh && cmd > 0 && cmd < 0x79) {
                        vocalTicks.append(curTick);
                    }
                    curTick += dur;
                    pos += 2;
                }

                pos = noteStart;
                curTick = 0;
                while (curTick < endTick && pos + 1 < fileSize) {
                    curTick += d[pos + 1]; pos += 2;
                }
                if (pos + 1 >= fileSize) break;
                int pc = d[pos] | (d[pos + 1] << 8); pos += 2 + pc * 4;
                if (pos + 1 >= fileSize) break;
                int vc = d[pos] | (d[pos + 1] << 8); pos += 2 + vc * 4;
                if (pos + 1 >= fileSize) break;
                int ptc = d[pos] | (d[pos + 1] << 8); pos += 2 + ptc * 4;
            }
            if (!vocalTicks.isEmpty()) {
                useFallback = false;
            }
        }
    }

    double offset = delayTicks;

    if (!useFallback) {
        // Filter out metadata lines from lyrics to identify the real first singing line
        QStringList metaKeywords = {
            "bass", "string", "tring", "cello", "drum", "piano", "guitar", "brass", 
            "synth", "flute", "elc", "inst", "horn", "violin", "organ", "sax", "trumpet"
        };
        int realFirstLineIdx = -1;
        for (int idx = 0; idx < lyrics.size(); ++idx) {
            QString line = lyrics[idx];
            QString lowerLine = line.toLower();
            bool isMeta = false;
            for (const QString& kw : metaKeywords) {
                if (lowerLine.contains(kw)) {
                    isMeta = true;
                    break;
                }
            }
            if (line.contains('$') || line.contains('#')) {
                isMeta = true;
            }
            
            int alphaCount = 0;
            for (const QChar& ch : line) {
                if (ch.isLetter()) {
                    alphaCount++;
                }
            }
            if (alphaCount < 2) {
                isMeta = true;
            }
            
            if (!isMeta) {
                realFirstLineIdx = idx;
                break;
            }
        }

        // Count syllables before the real first lyric line
        int unitsBefore = 0;
        if (realFirstLineIdx > 0) {
            for (int idx = 0; idx < realFirstLineIdx; ++idx) {
                for (const QChar& ch : lyrics[idx]) {
                    if (ch != ' ' && ch != '-' && ch != '@') {
                        unitsBefore++;
                    }
                }
            }
        }

        // Anchor the first real lyric character to the first vocal note
        int anchorIdx = unitsBefore;
        if (anchorIdx >= finalTicks.size()) {
            anchorIdx = 0;
        }

        unsigned long startVocalTick = vocalTicks[0];
        // Bypassing sparse intro notes (detecting dense clusters)
        int maxGap = (int)tb_div * 2;
        if (vocalTicks.size() >= 3) {
            for (int k = 0; k < vocalTicks.size() - 2; ++k) {
                int gap1 = (int)vocalTicks[k+1] - (int)vocalTicks[k];
                int gap2 = (int)vocalTicks[k+2] - (int)vocalTicks[k+1];
                if (gap1 <= maxGap && gap2 <= maxGap) {
                    startVocalTick = vocalTicks[k];
                    break;
                }
            }
        }

        unsigned long anchorLyricRaw = finalTicks[anchorIdx];
        offset = (double)startVocalTick - (double)anchorLyricRaw * scaleFactor;

        qDebug() << "[GybFileHandler] Optimal Anchor Detected: real_first_line_idx=" << realFirstLineIdx
                 << "anchor_idx=" << anchorIdx << "raw_tick=" << anchorLyricRaw
                 << "anchored to vocal note tick:" << startVocalTick
                 << "offset calibrated to:" << offset;
    } else {
        qDebug() << "[GybFileHandler] Fallback to standard constant delay offset:" << offset;
    }

    // 6. Map all lyric ticks linearly using the calibrated offset and tempo scale factor.
    //    We NO LONGER SNAP subsequent ticks to prevent melody snapping errors.
    unsigned long leadTicks = (unsigned long)qRound(playHz * 0.55);
    QList<unsigned long> scaledTicks;
    unsigned long prev = 0;

    for (unsigned long tk : finalTicks) {
        double linearTick = (double)tk * scaleFactor + offset;
        if (linearTick < 0.0) linearTick = 0.0;
        unsigned long v = (unsigned long)qRound(linearTick);

        // Apply 0.25-second lead display bias for karaoke highlight responsiveness
        if (v > leadTicks) {
            v -= leadTicks;
        } else {
            v = 0;
        }

        if (v < prev) v = prev; // enforce monotonic non-decreasing
        scaledTicks.append(v);
        prev = v;
    }

    qDebug() << "[GybFileHandler] Proportional Linear Sync completed. scale:" << scaleFactor
             << "offset:" << offset << "leadTicks:" << leadTicks;

    return scaledTicks;
}

QList<unsigned long> GybFileHandler::extractLyricSyllableTicks(const QString& filePath)
{
    QList<unsigned long> ticks;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return ticks;
    QByteArray data = file.readAll();
    file.close();
    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE) return ticks;

    QList<ChannelInfo> channels = parseChannelTable(filePath);
    if (channels.isEmpty()) return ticks;

    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    int count = d[GYB_INST_COUNT_OFFSET] | (d[GYB_INST_COUNT_OFFSET + 1] << 8);
    if (count <= 0 || count > 64) count = 13;
    const ChannelInfo& lastCh = channels.last();
    int instStart = lastCh.pitchOffset + lastCh.pitchCount * 4;
    int lyricStart = instStart + count * GYB_INST_RECORD_SIZE;
    if (lyricStart >= fileSize) return ticks;

    int tbDiv = d[0x28];
    if (tbDiv < 1 || tbDiv > 64) tbDiv = 4;

    // --- Lyric byte-scroll rate, reverse-engineered from GAYOBANG.EXE -----------
    // The DOS player shows lyrics LINE by line; each line occupies a fixed tick
    // window of  ticksPerLine = tbDiv * beatsPerLine * 4  (decomp FUN_28a1_0436
    // reads beatsPerLine = file[0x2A]; FUN_2dcf_2fe8: 167e = tbDiv*beatsPerLine,
    // then *8d56(=4) = ticksPerLine). Lyric lines are fixed 75-byte records, so the
    // highlight sweeps one byte/cell every ticksPerLine/75 ticks. Therefore
    //     tick(unit) = byteOffset * tbDiv * beatsPerLine * 4 / 75 .
    // The previous 256/1200 == 16/75 hard-coded beatsPerLine = 4, which is wrong for
    // songs where file[0x2A] != 4 (English P_G_005..010 use 3, P_G_004 uses 2): those
    // drifted late. For beatsPerLine == 4 it is byte-identical, so Korean songs that
    // already synced perfectly stay unchanged.
    int beatsPerLine = d[0x2A];
    if (beatsPerLine < 1 || beatsPerLine > 64) beatsPerLine = 4;
    const qint64 rateNum = (qint64)tbDiv * beatsPerLine * 4;   // GYB ticks per 75 bytes
    auto tickAt = [&](qint64 byteOff) -> unsigned long {
        return (unsigned long)(byteOff * rateNum / 75);
    };

    // Korean GYB stores lyrics in Johab (bytes >= 0x80, 2 bytes/syllable); export
    // (English) GYB stores them in plain ASCII (1 byte/cell). Detect which by
    // scanning for any Johab byte, then emit one marker per displayed unit at its
    // byte offset. The DOS highlight is a continuous left-to-right cell sweep, so a
    // unit's tick is just its byte offset times the scroll rate above.
    bool hasJohab = false;
    for (int i = lyricStart; i < fileSize; ++i) {
        if (d[i] >= 0x80) { hasJohab = true; break; }
    }

    if (hasJohab) {
        // Korean: one marker per Hangul syllable (2-byte Johab cell).
        for (int i = lyricStart; i < fileSize; ) {
            unsigned char b = d[i];
            if (b >= 0x80 && i + 1 < fileSize) {
                ticks.append(tickAt((qint64)(i - lyricStart)));
                i += 2;
            } else {
                i += 1;
            }
        }
    } else {
        // English (ASCII): one marker per printable character (each cell), excluding
        // spaces, hyphens (sustain) and '@'. The highlight sweeps character by
        // character (progressive), and the count matches the displayed per-character
        // units so setSyllableProgress stays aligned.
        //
        // The raw byte-scroll runs ~one beat behind the voice. ticksPerBeat == tbDiv
        // (tickHz = bpm*tbDiv/60, beats/s = bpm/60), and a half-beat lead (tbDiv/2)
        // still left it about half a beat late, so subtract a full one-beat lead.
        // Tune via the multiplier below if needed.
        // Lead so the first words line up, plus a tiny global speed-up because the
        // highlight otherwise progresses a hair slow. progressPct scales the rate and
        // pivots at song start, so the first words barely move while later words
        // advance a little sooner (start stays put, progression gets faster). Tune
        // progressPct (1000 = no change; smaller = faster).
        // Lines are separated in the stream by runs of NUL bytes (same split as
        // extractLyrics). Each line's first syllable carries extra leading-space delay,
        // so the highlight reaches a new line a touch late. Give the FIRST syllable of
        // every line an extra lead so the line transition lands on time, while in-line
        // syllables keep the normal lead (their pacing is already good).
        unsigned long leadTicks = (unsigned long)qMax(1, tbDiv * 7 / 4);       // 1.75 beats (in-line)
        unsigned long lineLead  = (unsigned long)qMax(0, tbDiv);               // +1 beat at line starts
        const qint64 progressPct = 989;   // ~1.1% faster progression
        int nullRun = 0;
        bool lineStart = true;            // first syllable of the lyric is a line start
        unsigned long prevTk = 0;
        for (int i = lyricStart; i < fileSize; ++i) {
            unsigned char b = d[i];
            if (b == 0) { nullRun++; if (nullRun >= 5) lineStart = true; continue; }
            nullRun = 0;
            if (b >= 0x21 && b <= 0x7E && b != '-' && b != '@') {
                qint64 byteOff = (qint64)(i - lyricStart);
                unsigned long tk = (unsigned long)(byteOff * rateNum * progressPct / (75 * 1000));
                unsigned long lead = leadTicks + (lineStart ? lineLead : 0);
                unsigned long v = (tk > lead) ? tk - lead : 0;
                if (v < prevTk) v = prevTk;        // keep non-decreasing
                ticks.append(v);
                prevTk = v;
                lineStart = false;
            }
        }
    }
    return ticks;
}

QList<unsigned long> GybFileHandler::extractLyricLineTicks(const QString& filePath, const QStringList& lyrics)
{
    QList<unsigned long> lineTicks;
    if (lyrics.isEmpty()) return lineTicks;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return lineTicks;
    QByteArray data = file.readAll();
    file.close();
    const int fileSize = data.size();
    if (fileSize < GYB_HEADER_SIZE) return lineTicks;

    QList<ChannelInfo> channels = parseChannelTable(filePath);
    if (channels.isEmpty()) return lineTicks;

    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.constData());
    int count = d[GYB_INST_COUNT_OFFSET] | (d[GYB_INST_COUNT_OFFSET + 1] << 8);
    if (count <= 0 || count > 64) count = 13;
    const ChannelInfo& lastCh = channels.last();
    int instStart = lastCh.pitchOffset + lastCh.pitchCount * 4;
    int lyricStart = instStart + count * GYB_INST_RECORD_SIZE;
    if (lyricStart >= fileSize) return lineTicks;

    QByteArray lyricData = data.mid(lyricStart);
    const int totalBytes = lyricData.size();
    if (totalBytes <= 0) return lineTicks;

    // -------------------------------------------------------------------------
    // 1. Detect each lyric LINE's start byte position in the stream.
    //    Mirrors extractLyrics()'s line-splitting (a line break = 5+ NULLs after
    //    visible content). lineStartByte[i] = byte offset of line i's first char.
    // -------------------------------------------------------------------------
    QList<int> lineStartByte;
    {
        int nullCount = 0;
        int curStart = -1;
        bool curHasContent = false;
        for (int i = 0; i < totalBytes; ) {
            unsigned char b = (unsigned char)lyricData[i];
            if (b == 0x00) {
                nullCount++;
                i++;
            } else {
                if (nullCount >= 5) {
                    if (curHasContent && curStart >= 0) lineStartByte.append(curStart);
                    curStart = -1;
                    curHasContent = false;
                }
                nullCount = 0;
                if (curStart < 0) curStart = i;
                if (b >= 0x80 || b != 0x20) curHasContent = true; // non-space = real content
                i += (b >= 0x80 && i + 1 < totalBytes) ? 2 : 1;
            }
        }
        if (curHasContent && curStart >= 0) lineStartByte.append(curStart);
    }

    // -------------------------------------------------------------------------
    // 2. Song end tick (max channel endTick). Re-parse payload header for it.
    //    NOTE: melody-based first-vocal anchoring was tried and REMOVED — the
    //    "first dense cluster" detector reliably mis-fires on dense INSTRUMENTAL
    //    intro notes (BACKPUSA Ch3 intro arpeggio → tick 139 instead of vocal
    //    248) and detectMelodyChannel picks wrong channels (NAMYU → Ch4 onset
    //    at tick 5). That made the first line worse, not better. Pure byte-
    //    proportional mapping (lyric byte fraction → song-tick fraction) is more
    //    robust across all songs and needs no melody detection.
    // -------------------------------------------------------------------------
    int songEndTick = 0;
    {
        int magic = d[GYB_MAGIC_OFFSET];
        int pos = (magic == GYB_MAGIC_VALUE_B) ? GYB_PAYLOAD_OFFSET_V3 : GYB_PAYLOAD_OFFSET_V4;
        if (pos + 2 <= fileSize) {
            int gc = d[pos] | (d[pos + 1] << 8); pos += 2;
            if (gc > 1000) gc = 0;
            pos += gc * 4;
            for (int ch = 0; ch < GYB_NUM_CHANNELS; ++ch) {
                if (pos + 3 >= fileSize) break;
                if (d[pos] != ch) break;
                pos += 1;
                int endTick = d[pos] | (d[pos + 1] << 8); pos += 2;
                if (endTick > songEndTick) songEndTick = endTick;
                int curTick = 0;
                while (curTick < endTick && pos + 1 < fileSize) {
                    curTick += d[pos + 1];
                    pos += 2;
                }
                if (pos + 1 >= fileSize) break;
                int pc = d[pos] | (d[pos + 1] << 8); pos += 2 + pc * 4;
                if (pos + 1 >= fileSize) break;
                int vc = d[pos] | (d[pos + 1] << 8); pos += 2 + vc * 4;
                if (pos + 1 >= fileSize) break;
                int ptc = d[pos] | (d[pos + 1] << 8); pos += 2 + ptc * 4;
            }
        }
    }
    if (songEndTick <= 0) songEndTick = totalBytes; // degenerate fallback

    // -------------------------------------------------------------------------
    // 3. Pure byte-proportional mapping: lyric byte fraction → song-tick fraction.
    //    line tick[i] = lineStartByte[i] / totalBytes * songEndTick
    //    The playback engine applies the per-song tempo events when converting
    //    these ticks to wall-clock, so intro-skip / tempo-scale are handled.
    // -------------------------------------------------------------------------
    if (lineStartByte.isEmpty()) return lineTicks;

    QList<unsigned long> mapped;
    unsigned long prev = 0;
    for (int idx = 0; idx < lineStartByte.size(); ++idx) {
        double t = (double)lineStartByte[idx] / (double)totalBytes * (double)songEndTick;
        if (t < 0.0) t = 0.0;
        unsigned long v = (unsigned long)qRound(t);
        if (v < prev) v = prev; // enforce monotonic non-decreasing
        mapped.append(v);
        prev = v;
    }

    // -------------------------------------------------------------------------
    // 4. Reconcile line count with the displayed lyric list. If our byte-based
    //    line detection disagrees with extractLyrics()'s line count, fall back
    //    to an even proportional distribution so indices stay aligned.
    // -------------------------------------------------------------------------
    if (mapped.size() == lyrics.size()) {
        lineTicks = mapped;
    } else {
        int n = lyrics.size();
        for (int idx = 0; idx < n; ++idx) {
            double frac = (n > 0) ? (double)idx / (double)n : 0.0;
            lineTicks.append((unsigned long)qRound(frac * (double)songEndTick));
        }
    }

    // -------------------------------------------------------------------------
    // 5. Karaoke-style LEAD bias. The lyric stream uses fixed-width (75-byte)
    //    line records, so byte position only encodes line INDEX, not real
    //    timing — the distribution is necessarily uniform while real singing has
    //    variable line durations (intro lines come quicker). Biasing every line
    //    EARLIER by 0.4 x average line interval makes the highlight lead rather
    //    than lag, matching karaoke convention (you see the line just before
    //    singing it) and halving the perceived uniform-vs-variable error.
    // -------------------------------------------------------------------------
    if (lineTicks.size() >= 2) {
        double avgInterval = (double)(lineTicks.last() - lineTicks.first())
                           / (double)(lineTicks.size() - 1);
        long lead = (long)qRound(avgInterval * 0.55);
        for (int i = 0; i < lineTicks.size(); ++i) {
            long v = (long)lineTicks[i] - lead;
            if (v < 0) v = 0;
            lineTicks[i] = (unsigned long)v;
        }
    }

    return lineTicks;
}

QByteArray GybFileHandler::readWholeFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    QByteArray data = file.readAll();
    file.close();
    return data;
}

QByteArray GybFileHandler::extractEventStream(const QString& filePath)
{
    // Legacy entry point — returns the full post-header bytes.
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QByteArray();
    if (file.size() < GYB_HEADER_SIZE) { file.close(); return QByteArray(); }
    file.seek(GYB_HEADER_SIZE);
    QByteArray streamData = file.readAll();
    file.close();
    return streamData;
}

QString GybFileHandler::decodeJohab(const QByteArray& data)
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
