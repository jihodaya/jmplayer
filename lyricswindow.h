#ifndef LYRICSWINDOW_H
#define LYRICSWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QTimer>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QApplication>
#include <QScreen>
#include <QComboBox>
#include <QPushButton>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

class LyricsWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit LyricsWindow(QWidget *parent = nullptr);
    ~LyricsWindow();

    // 가사 설정
    void setLyrics(const QStringList& lyrics);
    void clearLyrics();
    void setTitle(const QString& title);
    void setCurrentFilePath(const QString& filePath);
    QString currentFilePath() const { return m_currentFilePath; }

    // 재생 제어
    void setCurrentLine(int lineIndex);
    void setProgress(double percentage); // 0.0 ~ 1.0
    void reset();

    // NOB 파일 전용 - 음절별 하이라이팅
    void highlightSyllable(int lineIndex, int syllableIndex);
    void setSyllableProgress(int totalSyllableIndex); // 전체 음절 인덱스로 하이라이팅
    // Continuous/smooth variant: prog = lastSungIndex + frac(0..1 toward next).
    // Renders the current line discretely (unchanged look) but fades the next
    // line's first syllable in across the inter-line gap so transitions glide.
    void setSyllableProgressF(double prog);
    // Fade a line's first syllable white -> yellow by frac (smooth line entry).
    // Also used by the ISS/IMS path to fade the next display line in across the gap.
    void renderFirstSyllableFade(int lineIdx, double frac);

    // ISS 파일 전용 - 음절별 실시간 하이라이팅
    void setIssHighlight(int lineIndex, const QVector<bool>& highlightMask);

    // NOB 채널 선택
    void setNobFile(bool isNob);
    void setChannelWidgetVisible(bool visible); // 채널 선택 콤보박스 위젯 보이기/숨기기
    void setCurrentChannel(int channel); // 1-based (1~16)
    int getCurrentChannel() const { return m_currentChannel; }

    // 위치 조정
    void positionBesideMainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

signals:
    void closed();
    void channelChanged(int newChannel); // NOB 채널 변경 시그널
    void lyricsEdited(const QStringList& newLyrics);

private slots:
    void onChannelSelectionChanged(int index);
    void onEditLyrics();

private:
    void setupUI();
    void updateDisplay();
    QString sanitizeForDisplay(const QString& line) const;
    int countUnitsInLine(const QString& line) const;
    void updateChannelSelection();
    QString processLyricLine(const QString& line, int lineIndex, double progress = 0.0);
    QString externalLyricsFilePath() const;
    bool writeExternalLyrics(const QStringList& lyrics);
    bool exportLyricsToFile(const QStringList& lyrics, QWidget *parent);

    QWidget *parentMainWindow;

    // UI 컴포넌트
    QVBoxLayout *mainLayout;
    QScrollArea *scrollArea;
    QWidget *contentWidget;
    QVBoxLayout *contentLayout;

    QLabel *titleLabel;
    QList<QLabel*> lyricLabels;
    QPushButton *editButton;

    // NOB 채널 선택 UI
    QWidget *m_channelWidget;
    QLabel *m_channelLabel;
    QComboBox *m_channelCombo;

    // 가사 데이터
    QStringList allLyrics;
    QStringList m_displayLyrics;
    QVector<int> m_lineUnitCounts;
    int currentLineIndex;
    double currentProgress;
    bool m_hasRepeatToStart;
    int m_totalUnitsPerCycle;
    int m_smoothLastM = -2;   // last integer index rendered by setSyllableProgressF

    // NOB 파일 관련
    bool m_isNobFile;
    int m_currentChannel; // 1-based
    QString m_currentFilePath;

    // 스타일
    QString normalLineStyle;
    QString currentLineStyle;
    QString highlightedSyllableStyle;
    QString pastLineStyle;
};

#endif // LYRICSWINDOW_H
