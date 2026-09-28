#ifndef RCPFILEHANDLER_H
#define RCPFILEHANDLER_H

#include <QString>
#include <QByteArray>
#include <vector>
#include <cstdint>

/**
 * @brief Roland Recomposer (RCP/G36/G18/SNG) 파일 처리기
 *
 * 1990년대 일본 롤랜드(Roland) Recomposer 및 Ballade 시퀀서 데이터를
 * 표준 MIDI 파일(SMF Format 1)로 변환합니다.
 */
class RcpFileHandler
{
public:
    /**
     * @brief 파일이 RCP/G36/SNG 포맷인지 판별
     */
    static bool isRcpFile(const QString& filePath);
    static bool isRcpData(const QByteArray& data);

    /**
     * @brief RCP 파일에서 곡 제목 추출 (Shift-JIS / CP949 자동 판별)
     */
    static QString extractTitle(const QString& filePath);
    static QString extractTitle(const QByteArray& data);

    /**
     * @brief RCP 파일에서 메모/설명 추출
     */
    static QString extractMemo(const QString& filePath);
    static QString extractMemo(const QByteArray& data);

    /**
     * @brief RCP 파일을 파싱하여 표준 MIDI(SMF Format 1) 바이트 배열 생성
     */
    static QByteArray extractMidiData(const QString& filePath);
    static QByteArray extractMidiData(const QByteArray& rcpData);

private:
    // The real converter. `srcPath` is where the file came from, which is how
    // the .GSD / .CM6 control file named in the header gets found - a Recomposer
    // song carries no module setup of its own.
    static QByteArray convertRcp(const QByteArray& rcpData, const QString& srcPath);
};

#endif // RCPFILEHANDLER_H
