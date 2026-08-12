#ifndef OKAFILEHANDLER_H
#define OKAFILEHANDLER_H

#include <QString>
#include <QStringList>
#include <QByteArray>

/**
 * @brief OKA / OKM (Oksori Music File) 처리 유틸리티
 *
 * 옥소리 노래방(NORE45/PLAYER.EXE)의 네이티브 음악 포맷.
 *
 * 파일 구조 (reverse-engineered from NORE45.EXE):
 *   0x00          "Oksori Music File\0"  (평문 시그니처)
 *   0x1B          0x0A 검증 바이트
 *   0x27          제목 (평문 Johab/CP1361, NUL 종료)
 *   0x85          가사 미리보기 (평문 Johab)
 *   0x310         음악 데이터 = XOR 0xA8 암호화된 표준 MIDI 파일(SMF)
 *   (MIDI 뒤)     가사 타이밍 trailing 블록 (XOR 0xA8)
 *
 * 핵심: 음악은 0xA8로 XOR하면 그대로 표준 MIDI(MThd...)가 된다.
 * 따라서 기존 MidiPlayer로 NOB과 동일하게 재생할 수 있다.
 */
class OkaFileHandler
{
public:
    /// "Oksori Music File" 시그니처로 OKA/OKM/OKW 여부 확인
    static bool isOkaFile(const QString& filePath);

    /// 제목 추출 (0x27 평문 Johab). 없으면 빈 문자열.
    static QString extractTitle(const QString& filePath);
    static QString extractTitle(const QByteArray& fileData);

    /// 음악 데이터 추출: 0x310부터 XOR 0xA8 복호 → 표준 MIDI(SMF) 바이트.
    /// MThd + ntracks×MTrk 만큼 정확히 잘라서 반환 (trailing 가사 블록 제외).
    static QByteArray extractMidiData(const QString& filePath);

    /// 가사 추출 (0x85 평문 Johab 미리보기). 라인 단위 분할.
    /// NOTE: 전체 타임드 가사는 MIDI 뒤 trailing 블록에 있음 (추후 구현).
    static QStringList extractLyrics(const QString& filePath);

    /// OPL 악기 슬롯명 추출.
    ///
    /// OKA 는 SMF 뒤에 GYB 와 byte-identical 한 악기 테이블(레코드당 38바이트:
    /// 이름 9바이트 + OPL 오퍼레이터 파라미터 29바이트)을 임베드한다.
    /// SMF 의 Program Change 값은 GM 프로그램 번호가 아니라 이 테이블의
    /// **슬롯 인덱스**(0..N)다. 따라서 반환되는 QStringList 의 인덱스 = PC 값.
    ///
    /// 반환된 이름들은 GybBackend 와 동일하게 STANDARD.BNK 에서 대소문자 무시
    /// 매칭으로 OPL 패치를 해석하는 데 사용된다 (해석 불가 슬롯은 무음/기본).
    /// 슬롯을 찾지 못하면 빈 리스트.
    static QStringList extractInstrumentNames(const QString& filePath);

    /// The 28 OPL parameter bytes of each embedded instrument record, in the
    /// same order as extractInstrumentNames(). This is the instrument itself -
    /// the record layout matches a .BNK entry minus its mode/voice-number
    /// header, so the song needs no external bank at all.
    static QList<QByteArray> extractInstrumentParams(const QString& filePath);

    /// 가사 싱크 마커들의 틱 정보를 추출.
    /// 마커의 오프셋을 활용하여 전체 음절 수에 매핑된 정확한 틱 목록을 반환한다.
    static QList<unsigned long> extractLyricMarkerTicks(const QString& filePath);

    /// 헤더의 0x15a 오프셋에서 멜로디 채널 정보(0-indexed)를 읽어와 1-indexed 미디 채널 번호로 반환한다.
    static int extractMelodyChannel(const QString& filePath);

    /// 임베드된 SMF(0x310, XOR'd)의 MThd division(ticks-per-quarter)을 반환. 실패 시 0.
    /// GYB 가사 싱크를 OKA 싱크 데이터로 구동할 때 tick rate 변환에 사용.
    static int getMidiTicksPerQuarter(const QString& filePath);

    /// 조합형 한글(Johab/CP1361) 디코딩 (Windows MultiByteToWideChar 1361)
    static QString decodeJohab(const QByteArray& data);

private:
    static constexpr int  OKA_MUSIC_OFFSET = 0x310;  // 음악(XOR'd SMF) 시작
    static constexpr int  OKA_TITLE_OFFSET = 0x27;   // 제목 (Johab)
    static constexpr int  OKA_LYRIC_OFFSET = 0x85;   // 가사 미리보기 (Johab)
    static constexpr unsigned char OKA_XOR_KEY = 0xA8;
    static constexpr int  OKA_INST_RECORD_LEN = 38;  // 악기 레코드 크기 (이름9 + OPL29)
    static constexpr int  OKA_INST_NAME_LEN   = 9;   // 슬롯 이름 필드 길이

    /// 조합형 한글 디코딩을 수행하면서 원본 바이트 오프셋을 맵핑한다.
    static QString decodeJohabWithMapping(const QByteArray& data, int startOffset, QList<int>& charToByteOffset);
};

#endif // OKAFILEHANDLER_H
