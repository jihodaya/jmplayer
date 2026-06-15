#ifndef ISSFILEHANDLER_H
#define ISSFILEHANDLER_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QFile>

class IssFileHandler
{
public:
    struct SongRec {
        uint16_t kasa_tick; // IMS 틱 / 8
        uint8_t  line;      // 가사 줄 번호 (0-based)
        uint8_t  start_x;   // 줄에서의 문자 시작 위치
        uint8_t  width_x;   // 색칠할 문자 수
        int      char_start; // 유니코드 문자 시작 인덱스
        int      char_width; // 유니코드 문자 너비
    };

    struct IssData {
        QString writer;
        QString composer;
        QString singer;
        QString editor;

        QStringList lines;          // 전체 가사 줄 목록 (line_count개)
        QList<SongRec> records;     // 원본 레코드 전체
        int tickMultiplier = 8;

        // 가사창에 표시할 줄 목록 (재생 순서대로, 후렴 반복 포함)
        QStringList displayLines;
        // displayLines[i]에 해당하는 원본 ISS line 번호 (0-based)
        QList<int> displayLineSource;
        // displayLines[i]에 해당하는 시작 시간 (ms)
        QList<unsigned long> displayLineMs;
    };

    // basicTempo, nTickBeat 는 IMS 헤더에서 읽어온 값
    static IssData loadIssFile(const QString &filePath,
                               int basicTempo = 120,
                               int nTickBeat  = 240);
};

#endif // ISSFILEHANDLER_H
