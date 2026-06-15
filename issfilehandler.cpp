#include "issfilehandler.h"
#include <QDebug>
#include <windows.h>

// CP1361 바이트 오프셋 -> 유니코드 문자 인덱스 매핑 테이블 빌더
static QVector<int> buildByteToCharMap(const char* data, int len) {
    QVector<int> map(len + 1, 0);
    int rawIdx = 0;
    int charIdx = 0;

    while (rawIdx < len) {
        map[rawIdx] = charIdx;
        unsigned char c1 = (unsigned char)data[rawIdx];
        if (c1 == '\0') {
            break;
        }

        if (c1 >= 0x80) { // 한글 및 특수기호 (2바이트)
            if (rawIdx + 1 < len) {
                unsigned char c2 = (unsigned char)data[rawIdx+1];
                char buf[2] = { (char)c1, (char)c2 };
                wchar_t wbuf[2] = {0};
                int res = MultiByteToWideChar(1361, 0, buf, 2, wbuf, 2);
                if (res == 0 || wbuf[0] == L'?' || wbuf[0] == 0xFFFD || 
                   (wbuf[0] >= 0xE000 && wbuf[0] <= 0xF8FF)) {
                    // DOS 전용 PUA 폰트 기호로 인해 스킵된 경우
                    map[rawIdx] = charIdx;
                    map[rawIdx+1] = charIdx;
                    rawIdx += 2;
                    continue;
                }
                map[rawIdx] = charIdx;
                map[rawIdx+1] = charIdx;
                rawIdx += 2;
                charIdx++;
            } else {
                map[rawIdx] = charIdx;
                rawIdx++;
                charIdx++;
            }
        } else { // 1바이트 ASCII
            map[rawIdx] = charIdx;
            rawIdx++;
            charIdx++;
        }
    }
    // 남은 오프셋 영역 채우기
    for (int i = rawIdx; i <= len; ++i) {
        map[i] = charIdx;
    }
    return map;
}

// Johab (CP1361) 조합형 → 유니코드 변환
static QString decodeJohab(const char* data, int len) {
    if (!data || len <= 0) return QString();

    // CP1361(Windows Johab)에서 변환 실패 시 '?'로 대체되는 DOS 특수문자들 필터링
    QByteArray cleanData;
    cleanData.reserve(len);
    for (int i = 0; i < len; ++i) {
        unsigned char c1 = (unsigned char)data[i];
        if (c1 >= 0x80) {
            if (i + 1 < len) {
                unsigned char c2 = (unsigned char)data[i+1];
                // 2바이트를 먼저 변환해보고 '?'가 나오거나
                // 유니코드 사용자 정의 영역(PUA: 0xE000 ~ 0xF8FF)으로 매핑되면 DOS 전용 글꼴 기호이므로 무시
                char buf[2] = { (char)c1, (char)c2 };
                wchar_t wbuf[2] = {0};
                int res = MultiByteToWideChar(1361, 0, buf, 2, wbuf, 2);
                if (res == 0 || wbuf[0] == L'?' || wbuf[0] == 0xFFFD || 
                   (wbuf[0] >= 0xE000 && wbuf[0] <= 0xF8FF)) {
                    i++; // 스킵
                    continue;
                }
                cleanData.append(c1);
                cleanData.append(c2);
                i++;
            } else {
                cleanData.append(c1);
            }
        } else {
            cleanData.append(c1);
        }
    }

    int wideLen = MultiByteToWideChar(1361, 0, cleanData.constData(), cleanData.size(), NULL, 0);
    if (wideLen <= 0) return QString::fromLocal8Bit(cleanData.constData(), cleanData.size());
    wchar_t* wideBuf = new wchar_t[wideLen + 1];
    MultiByteToWideChar(1361, 0, cleanData.constData(), cleanData.size(), wideBuf, wideLen);
    wideBuf[wideLen] = 0;
    QString result = QString::fromWCharArray(wideBuf);
    delete[] wideBuf;
    return result;
}

static QString decodeJohabBytes(const QByteArray& b) {
    // null 전까지만 사용
    int len = 0;
    while (len < b.size() && b[len] != '\0') ++len;
    return decodeJohab(b.constData(), len);
}

IssFileHandler::IssData IssFileHandler::loadIssFile(
        const QString &filePath, int basicTempo, int nTickBeat)
{
    IssData result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return result;

    QByteArray data = file.readAll();
    file.close();

    // ─── 헤더 (공식 구조, 154 bytes = 0x9A) ──────────────────────
    // head_str[20] + reserved[10] + writer[30] + composer[30]
    // + singer[30] + editor[30] + rec_count(2) + line_count(2)
    if (data.size() < 154) return result;

    result.writer   = decodeJohabBytes(data.mid(30, 30));
    result.composer = decodeJohabBytes(data.mid(60, 30));
    result.singer   = decodeJohabBytes(data.mid(90, 30));
    result.editor   = decodeJohabBytes(data.mid(120, 30));

    uint16_t rec_count  = 0;
    uint16_t line_count = 0;
    memcpy(&rec_count,  data.constData() + 150, 2);
    memcpy(&line_count, data.constData() + 152, 2);

    qDebug() << "[IssFileHandler]" << filePath;
    qDebug() << "  rec_count=" << rec_count << "  line_count=" << line_count;
    qDebug() << "  basicTempo=" << basicTempo << "  nTickBeat=" << nTickBeat;

    // ─── 가사 줄 및 매핑 테이블 구축 ───────────────────────────────
    int lineAreaStart = 154 + rec_count * 5;
    QList<QVector<int>> lineByteToCharMaps;

    for (int i = 0; i < line_count; ++i) {
        int off = lineAreaStart + i * 64;
        if (off + 64 > data.size()) break;

        QByteArray rawLine = data.mid(off, 64);
        int len = 0;
        while (len < rawLine.size() && rawLine[len] != '\0') ++len;

        QString decoded = decodeJohab(rawLine.constData(), len);
        result.lines.append(decoded);

        QVector<int> map = buildByteToCharMap(rawLine.constData(), len);
        lineByteToCharMaps.append(map);
    }

    // ─── 레코드 (154부터, 5바이트씩) ───────────────────────────────
    if (data.size() < lineAreaStart) return result;

    for (int i = 0; i < rec_count; ++i) {
        int off = 154 + i * 5;
        if (off + 5 > data.size()) break;
        SongRec rec;
        memcpy(&rec.kasa_tick, data.constData() + off, 2);
        rec.line    = static_cast<uint8_t>(data[off+2]);
        rec.start_x = static_cast<uint8_t>(data[off+3]);
        rec.width_x = static_cast<uint8_t>(data[off+4]);

        // 유니코드 좌표 매핑 계산
        if (rec.line < result.lines.size() && rec.line < lineByteToCharMaps.size()) {
            const QVector<int>& map = lineByteToCharMaps[rec.line];
            int start_byte = rec.start_x;
            int end_byte = start_byte + rec.width_x;

            if (start_byte >= map.size()) start_byte = map.size() - 1;
            if (end_byte >= map.size()) end_byte = map.size() - 1;

            if (start_byte < 0) start_byte = 0;
            if (end_byte < 0) end_byte = 0;

            rec.char_start = map[start_byte];
            rec.char_width = map[end_byte] - rec.char_start;
        } else {
            rec.char_start = 0;
            rec.char_width = 0;
        }

        result.records.append(rec);
    }

    qDebug() << "  lines loaded:" << result.lines.size();
    for (int i = 0; i < result.lines.size(); ++i)
        qDebug() << "   Line[" << i << "]:" << result.lines[i];

    // 한글 포함 여부 검사 (8x vs 10x 판정)
    bool hasKorean = false;
    for (const QString& line : result.lines) {
        for (const QChar& ch : line) {
            ushort u = ch.unicode();
            if ((u >= 0xAC00 && u <= 0xD7A3) || (u >= 0x3130 && u <= 0x318F)) {
                hasKorean = true;
                break;
            }
        }
        if (hasKorean) break;
    }
    result.tickMultiplier = hasKorean ? 8 : 10;
    qDebug() << "[IssFileHandler] Detected tick multiplier:" << result.tickMultiplier 
             << "for" << filePath << "(hasKorean=" << hasKorean << ")";

    // ─── 표시용 가사 목록 생성 ──────────────────────────────────
    // 레코드를 순서대로 처리하면서 줄 번호가 바뀌는 시점에 새 항목 추가
    // 시간 공식 (Main.cpp에서 확인):
    //   ms = kasa_tick * tickMultiplier * 60000 / (basicTempo * nTickBeat)
    int prevLineIdx = -1;
    double msPerTickMultiplier = 60000.0 / (double)(basicTempo * nTickBeat);

    for (const SongRec& rec : result.records) {
        if (rec.line >= result.lines.size()) continue;
        if (rec.line != prevLineIdx) {
            result.displayLines.append(result.lines[rec.line]);
            result.displayLineSource.append(rec.line);  // 원본 ISS 줄 번호 보존
            unsigned long ms = (unsigned long)(rec.kasa_tick * result.tickMultiplier * msPerTickMultiplier);
            result.displayLineMs.append(ms);
            prevLineIdx = rec.line;
        }
    }

    qDebug() << "  displayLines:" << result.displayLines.size();
    for (int i = 0; i < result.displayLines.size(); ++i)
        qDebug() << "   [" << i << "] ms=" << result.displayLineMs[i]
                 << " '" << result.displayLines[i] << "'";

    return result;
}
