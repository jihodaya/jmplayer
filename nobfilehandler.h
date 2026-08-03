#ifndef NOBFILEHANDLER_H
#define NOBFILEHANDLER_H

#include <QString>
#include <QStringList>
#include <QByteArray>

/**
 * @brief NOB 파일 처리 유틸리티
 *
 * 옥소리 노래방 4.0 이전 버전의 NOB 파일을 처리합니다.
 * NOB 파일 구조:
 * - 헤더: 69 bytes (0x00-0x44)
 *   - 0x00: 플래그 (0x08)
 *   - 0x01-0x0F: 제목 (EUC-KR 인코딩)
 *   - 0x10-0x44: 설정 데이터
 * - MIDI 데이터: 0x45-EOF (표준 MIDI Format 1)
 */
class NobFileHandler
{
public:
    /**
     * @brief NOB 파일인지 확인
     * @param filePath 확인할 파일 경로
     * @return NOB 파일이면 true
     */
    static bool isNobFile(const QString& filePath);

    /**
     * @brief NOB 파일에서 제목 추출
     * @param filePath NOB 파일 경로
     * @return 제목 문자열 (EUC-KR 디코딩)
     */
    static QString extractTitle(const QString& filePath);

    /**
     * @brief NOB 파일에서 MIDI 데이터 추출
     * @param filePath NOB 파일 경로
     * @return MIDI 데이터 (표준 MIDI Format 1)
     */
    static QByteArray extractMidiData(const QString& filePath);

    /**
     * @brief NOB 헤더에서 제목 추출 (내부 함수)
     * @param header 69 bytes 헤더 데이터
     * @return 제목 문자열
     */
    static QString extractTitleFromHeader(const QByteArray& header);

    /**
     * @brief NOB.LST 파일에서 제목 추출
     * @param nobFilePath NOB 파일 경로
     * @return LST 파일에 있는 제목 (없으면 빈 문자열)
     */
    static QString extractTitleFromLst(const QString& nobFilePath);

    /**
     * @brief NOB 파일에서 가사 추출
     * @param filePath NOB 파일 경로
     * @return 가사 라인 리스트 (조합형 한글 디코딩)
     */
    static QStringList extractLyrics(const QString& filePath);
    static QStringList loadExternalLyrics(const QString& filePath, bool *found = nullptr);
    static bool saveExternalLyrics(const QString& filePath, const QStringList& lyrics);

    /**
     * @brief NOB 파일에서 MIDI 총 틱 수 계산
     * @param filePath NOB 파일 경로
     * @return 총 틱 수 (마지막 이벤트의 절대 시간)
     */
    static unsigned long calculateTotalTicks(const QString& filePath);

    /**
     * @brief NOB 파일에서 첫 노트 온 시점 계산
     * @param filePath NOB 파일 경로
     * @return 첫 노트 온 틱 (인트로 종료 시점, 가사 시작점)
     */
    static unsigned long calculateFirstNoteOnTick(const QString& filePath);

    /**
     * @brief NOB 파일에서 11번 채널 가사 마커 틱 추출
     * @param filePath NOB 파일 경로
     * @return 각 가사 라인 시작 틱 리스트 (비어있으면 마커 없음)
     * @note 첫 마커는 준비 신호이므로 제외, 나머지를 라인 수로 등분
     */
    static QList<unsigned long> extractLyricMarkerTicks(const QString& filePath);

    // Per-syllable ticks read from the lyric block's own layout, the way OKM
    // carries an explicit sync table. Empty when the file does not fit the
    // model. `guideTicks` are note ticks from the detected guide channel, used
    // only to calibrate the column scale.
    static QList<unsigned long> extractLyricColumnTicks(const QString& filePath,
                                                        const QList<unsigned long>& guideTicks);

    // Ticks per lyric column - see extractLyricColumnTicks() for how this was
    // measured.
    static double lyricTicksPerColumn();

    static QString decodeJohab(const QByteArray& data);

    /**
     * @brief NOB 파일의 마커 채널 자동 감지
     * @param filePath NOB 파일 경로
     * @return 마커 채널 번호 (1-16), 감지 실패 시 -1
     * @note 가사 단위 수와 Note On 이벤트 수의 비율(0.5~1.2)로 감지
     */
    static int detectMarkerChannel(const QString& filePath);

private:
    static constexpr int NOB_HEADER_SIZE = 69;       // 0x45
    static constexpr int NOB_TITLE_OFFSET = 1;       // 0x01
    static constexpr int NOB_TITLE_SIZE = 15;        // 0x01-0x0F
    static constexpr unsigned char NOB_FLAG = 0x08;  // 0x00의 값
    static constexpr int MIDI_OFFSET = 0x45;         // MIDI 시작 오프셋

};

#endif // NOBFILEHANDLER_H
