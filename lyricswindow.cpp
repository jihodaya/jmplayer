#include "lyricswindow.h"
#include "nobfilehandler.h"
#include <QCloseEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>
#include <QSaveFile>
#include <QTextStream>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif
#include <QPushButton>

LyricsWindow::LyricsWindow(QWidget *parent)
    : QMainWindow(parent)
    , parentMainWindow(parent)
    , currentLineIndex(-1)
    , currentProgress(0.0)
    , m_hasRepeatToStart(false)
    , m_totalUnitsPerCycle(0)
    , m_isNobFile(false)
    , m_currentChannel(11)
    , m_currentFilePath()
{
    // Ensure same DPI context as parent window
    if (parent) {
        setWindowFlags(windowFlags() | Qt::Tool);  // Inherit DPI from parent
    }
    setupUI();
    updateChannelSelection();
    // ?��????�의
    normalLineStyle = "QLabel { color: #CCCCCC; font-size: 14px; padding: 5px; border: none; background: transparent; }";
    currentLineStyle = "QLabel { color: #FFFF00; font-size: 16px; font-weight: bold; padding: 5px; background-color: #4a4a4a; border: none; }";
    highlightedSyllableStyle = "color: #FF6600;"; // 주황?�으�?강조
    pastLineStyle = "QLabel { color: #888888; font-size: 14px; padding: 5px; border: none; background: transparent; }";
    setWindowTitle(QStringLiteral(u"\u266A JJoMe MIDI Player - Lyrics"));
    QString iconPath = QApplication::applicationDirPath() + "/K_icon.ico";
    QIcon icon(iconPath);
    if (icon.isNull()) {
        icon = QIcon(":/K_icon.ico");
    }
    setWindowIcon(icon);
    // 배경???�정 - 채널모니?��? ?�일???�마
    setStyleSheet(
        "QMainWindow {"
        "    background-color: #2b2b2b;"
        "    color: #ffffff;"
        "}"
        "QScrollArea {"
        "    border: 1px solid #555555;"
        "    background-color: #2b2b2b;"
        "}"
        "QFrame {"
        "    background-color: #3a3a3a;"
        "    border: 1px solid #555555;"
        "}"
    );
    positionBesideMainWindow();
}
LyricsWindow::~LyricsWindow()
{
}
void LyricsWindow::setupUI()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(5);
    // ?�목 ?�벨
    QHBoxLayout *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    titleLabel = new QLabel(QStringLiteral(u"\u266C Lyrics"), centralWidget);
    titleLabel->setStyleSheet("QLabel { color: white; font-size: 16px; font-weight: bold; padding: 5px; }");
    titleLabel->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(titleLabel, 1);
    editButton = new QPushButton(tr("Edit Lyrics"), centralWidget);
    editButton->setToolTip(tr("Edit lyrics (~: hold 1 beat, #: delay 1 beat, @: repeat to start)"));
    editButton->setCursor(Qt::PointingHandCursor);
    editButton->setStyleSheet("QPushButton { color: #FFFFFF; background-color: #444444; border: 1px solid #666666; padding: 4px 12px; border-radius: 3px; } QPushButton:hover { background-color: #555555; } QPushButton:pressed { background-color: #333333; }");
    headerLayout->addWidget(editButton);
    connect(editButton, &QPushButton::clicked, this, &LyricsWindow::onEditLyrics);
    mainLayout->addLayout(headerLayout);
    // ?�크�??�역
    scrollArea = new QScrollArea(centralWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Style already set in main stylesheet
    contentWidget = new QWidget();
    contentWidget->setStyleSheet("QWidget { background-color: #2b2b2b; }"); // contentWidget�?배경???�정
    contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(10, 10, 10, 10);
    contentLayout->setSpacing(8);
    contentLayout->setAlignment(Qt::AlignTop);
    scrollArea->setWidget(contentWidget);
    mainLayout->addWidget(scrollArea);
    // NOB 채널 ?�택 UI (기본?�으�??��?)
    m_channelWidget = new QWidget(centralWidget);
    QHBoxLayout *channelLayout = new QHBoxLayout(m_channelWidget);
    channelLayout->setContentsMargins(5, 5, 5, 5);
    m_channelLabel = new QLabel("Lyric Channel:", m_channelWidget);
    m_channelLabel->setStyleSheet("QLabel { color: #CCCCCC; font-size: 12px; }");
    channelLayout->addWidget(m_channelLabel);
    m_channelCombo = new QComboBox(m_channelWidget);
    for (int channel = 1; channel <= 16; ++channel) {
        m_channelCombo->addItem(QString("Channel %1").arg(channel), channel);
    }
    m_channelCombo->setMaximumWidth(120);
    m_channelCombo->setStyleSheet(
        "QComboBox {"
        "    background-color: #3a3a3a;"
        "    color: #CCCCCC;"
        "    border: 1px solid #555555;"
        "    padding: 2px 6px;"
        "    font-size: 11px;"
        "}"
        "QComboBox::drop-down {"
        "    border: none;"
        "}"
        "QComboBox QAbstractItemView {"
        "    background-color: #2b2b2b;"
        "    color: #FFFFFF;"
        "    selection-background-color: #FF6600;"
        "}"
    );
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LyricsWindow::onChannelSelectionChanged);
    channelLayout->addWidget(m_channelCombo);
    channelLayout->addStretch();
    m_channelWidget->setVisible(false); // 기본?�으�??��?
    mainLayout->addWidget(m_channelWidget);
    // ?�기??조정 UI (기본?�으�??��?)
    #ifdef _WIN32
    {
        HWND hwnd = (HWND)winId();
        // Enable dark mode for window
        BOOL useDarkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode));
        BOOL useImmersiveDarkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 19, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode));
        // Match titlebar height and style with main window but remove minimize box
        LONG_PTR style = ::GetWindowLongPtr(hwnd, GWL_STYLE);
        style |= WS_CAPTION | WS_SYSMENU;
        style &= ~WS_MINIMIZEBOX; // Remove minimize button
        ::SetWindowLongPtr(hwnd, GWL_STYLE, style);
        // Force window refresh to apply changes
        ::SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    #endif
}
void LyricsWindow::setLyrics(const QStringList& lyrics)
{
    clearLyrics();
    allLyrics = lyrics;
    m_displayLyrics.clear();
    m_lineUnitCounts.clear();
    m_displayLyrics.reserve(lyrics.size());
    m_lineUnitCounts.reserve(lyrics.size());
    m_hasRepeatToStart = false;
    m_totalUnitsPerCycle = 0;
    // Debug: Show status
    qDebug() << "[LyricsWindow] setLyrics called with" << lyrics.size() << "lines";
    if (lyrics.isEmpty()) {
        // 가사 없음이면 아무것도 표시하지 않음 (하단 빈공간 유지)
        contentLayout->addStretch();
        return;
    }
    // Debug: Show first few lines
    qDebug() << "[LyricsWindow] First line:" << lyrics[0];
    if (lyrics.size() > 1) qDebug() << "[LyricsWindow] Second line:" << lyrics[1];
    // 가사 라벨 생성
    for (int i = 0; i < lyrics.size(); ++i)
    {
        const QString &line = lyrics[i];
        if (!m_hasRepeatToStart) {
            QString trimmedLine = line.trimmed();
            if (trimmedLine.endsWith('@')) {
                m_hasRepeatToStart = true;
            }
        }
        int unitCount = countUnitsInLine(line);
        m_lineUnitCounts.append(unitCount);
        m_totalUnitsPerCycle += unitCount;

        QString displayLine = sanitizeForDisplay(line);
        m_displayLyrics.append(displayLine);

        QLabel *label = new QLabel(displayLine, contentWidget);
        label->setWordWrap(true);
        label->setStyleSheet(normalLineStyle);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setTextFormat(Qt::RichText); // HTML 형식 지원
        lyricLabels.append(label);
        contentLayout->addWidget(label);
    }
    // 여백 추가
    contentLayout->addStretch();
    currentLineIndex = -1;
    currentProgress = 0.0;
    if (!m_hasRepeatToStart || m_totalUnitsPerCycle <= 0) {
        m_hasRepeatToStart = false;
    }
}

QString LyricsWindow::sanitizeForDisplay(const QString& line) const
{
    QString cleaned;
    cleaned.reserve(line.size());
    for (QChar ch : line) {
        if (ch == '@' || ch == '#') {
            continue;
        }
        cleaned.append(ch);
    }
    if (cleaned.trimmed().isEmpty()) {
        return QStringLiteral("&nbsp;");
    }
    return cleaned;
}

int LyricsWindow::countUnitsInLine(const QString& line) const
{
    int units = 0;
    for (QChar ch : line) {
        if (ch == ' ' || ch == '-' || ch == '@') {
            continue;
        }
        units++;
    }
    return units;
}

void LyricsWindow::clearLyrics()
{
    // 기존 ?�벨 ??��
    for (QLabel *label : lyricLabels)
    {
        contentLayout->removeWidget(label);
        delete label;
    }
    lyricLabels.clear();
    allLyrics.clear();
    m_displayLyrics.clear();
    m_lineUnitCounts.clear();
    // Remove all items including stretch
    QLayoutItem *item;
    while ((item = contentLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    currentLineIndex = -1;
    currentProgress = 0.0;
    m_hasRepeatToStart = false;
    m_totalUnitsPerCycle = 0;
    m_smoothLastM = -2;
}
void LyricsWindow::setCurrentLine(int lineIndex)
{
    if (lineIndex < 0 || lineIndex >= lyricLabels.size())
        return;
    // ?�전 ?�재 �??��???복원
    if (currentLineIndex >= 0 && currentLineIndex < lyricLabels.size())
    {
        lyricLabels[currentLineIndex]->setStyleSheet(pastLineStyle);
        QString fallback = (currentLineIndex < m_displayLyrics.size())
            ? m_displayLyrics[currentLineIndex]
            : sanitizeForDisplay(allLyrics[currentLineIndex]);
        lyricLabels[currentLineIndex]->setText(fallback);
    }
    // ?�로???�재 �?강조
    currentLineIndex = lineIndex;
    lyricLabels[currentLineIndex]->setStyleSheet(currentLineStyle);
    if (currentLineIndex < m_displayLyrics.size()) {
        lyricLabels[currentLineIndex]->setText(m_displayLyrics[currentLineIndex]);
    } else {
        lyricLabels[currentLineIndex]->setText(sanitizeForDisplay(allLyrics[currentLineIndex]));
    }
    // ?�크�??�동 조정 (?�재 줄이 중앙???�도�?
    QScrollBar *scrollBar = scrollArea->verticalScrollBar();
    QLabel *currentLabel = lyricLabels[currentLineIndex];
    int labelY = currentLabel->y();
    int scrollAreaHeight = scrollArea->height();
    int targetScroll = labelY - (scrollAreaHeight / 2) + (currentLabel->height() / 2);
    scrollBar->setValue(targetScroll);
}
void LyricsWindow::setProgress(double percentage)
{
    currentProgress = qBound(0.0, percentage, 1.0);
    if (allLyrics.isEmpty()) {
        return;
    }
    // Calculate which line should be current based on progress
    int totalLines = allLyrics.size();
    int newLineIndex = static_cast<int>(currentProgress * totalLines);
    if (newLineIndex >= totalLines) {
        newLineIndex = totalLines - 1;
    }
    // Update current line if changed
    if (newLineIndex != currentLineIndex && newLineIndex >= 0) {
        setCurrentLine(newLineIndex);
    }
    // Update current line display with syllable coloring
    if (currentLineIndex >= 0 && currentLineIndex < lyricLabels.size())
    {
        // Calculate progress within current line
        double lineStart = static_cast<double>(currentLineIndex) / totalLines;
        double lineEnd = static_cast<double>(currentLineIndex + 1) / totalLines;
        double lineProgress = 0.0;
        if (lineEnd > lineStart) {
            lineProgress = (currentProgress - lineStart) / (lineEnd - lineStart);
            lineProgress = qBound(0.0, lineProgress, 1.0);
        }
        QString processedLine = processLyricLine(allLyrics[currentLineIndex], currentLineIndex, lineProgress);
        lyricLabels[currentLineIndex]->setText(processedLine);
    }
}
void LyricsWindow::highlightSyllable(int lineIndex, int syllableIndex)
{
    if (lineIndex < 0 || lineIndex >= lyricLabels.size())
        return;
    if (lineIndex != currentLineIndex)
    {
        setCurrentLine(lineIndex);
    }
    // ?�절�??�상 처리
    QString line = allLyrics[lineIndex];
    QString processedLine = processLyricLine(line, lineIndex, syllableIndex);
    lyricLabels[lineIndex]->setText(processedLine);
}
void LyricsWindow::setSyllableProgress(int totalSyllableIndex)
{
    if (allLyrics.isEmpty()) {
        return;
    }

    int effectiveIndex = totalSyllableIndex;
    if (m_hasRepeatToStart && m_totalUnitsPerCycle > 0) {
        effectiveIndex = totalSyllableIndex % m_totalUnitsPerCycle;
    }
    if (effectiveIndex < 0) {
        effectiveIndex = 0;
    }

    int targetLine = -1;
    int targetCharInLine = 0;
    int remaining = effectiveIndex;

    for (int lineIdx = 0; lineIdx < allLyrics.size(); ++lineIdx) {
        int lineUnits = (lineIdx < m_lineUnitCounts.size())
            ? m_lineUnitCounts[lineIdx]
            : countUnitsInLine(allLyrics[lineIdx]);
        if (lineUnits <= 0) {
            continue;
        }
        if (remaining < lineUnits) {
            targetLine = lineIdx;
            targetCharInLine = remaining;
            break;
        }
        remaining -= lineUnits;
    }

    if (targetLine < 0 || targetLine >= allLyrics.size()) {
        return;
    }

    if (targetLine != currentLineIndex) {
        setCurrentLine(targetLine);
    }

    QString line = allLyrics[targetLine];

    // 1. Detect if the current line contains Korean characters
    bool lineHasKorean = false;
    for (const QChar& ch : line) {
        ushort code = ch.unicode();
        if ((code >= 0xAC00 && code <= 0xD7A3) || 
            (code >= 0x1100 && code <= 0x11FF) || 
            (code >= 0x3130 && code <= 0x318F)) {
            lineHasKorean = true;
            break;
        }
    }

    // 2. For English lines, map each character index to its word/token boundary
    QVector<int> wordStartIndices; // Start indices of words in beat-based index
    QVector<int> wordEndIndices;   // End indices of words (inclusive)
    
    if (!lineHasKorean) {
        int tempIdx = 0;
        bool inWord = false;
        int wordStart = -1;
        
        for (int i = 0; i < line.size(); ++i) {
            QChar ch = line[i];
            if (ch == ' ' || ch == '-' || ch == '@') {
                if (inWord) {
                    wordStartIndices.append(wordStart);
                    wordEndIndices.append(tempIdx - 1);
                    inWord = false;
                }
                continue;
            }
            if (ch == '#') {
                if (inWord) {
                    wordStartIndices.append(wordStart);
                    wordEndIndices.append(tempIdx - 1);
                    inWord = false;
                }
                tempIdx++;
                continue;
            }
            if (ch == '~') {
                if (inWord) {
                    wordStartIndices.append(wordStart);
                    wordEndIndices.append(tempIdx - 1);
                    inWord = false;
                }
                wordStartIndices.append(tempIdx);
                wordEndIndices.append(tempIdx);
                tempIdx++;
                continue;
            }
            
            if (!inWord) {
                inWord = true;
                wordStart = tempIdx;
            }
            tempIdx++;
        }
        if (inWord) {
            wordStartIndices.append(wordStart);
            wordEndIndices.append(tempIdx - 1);
        }
    }

    QString result;
    int charIndex = 0; // Beat-based index (special tokens excluded)
    for (int i = 0; i < line.size(); ++i) {
        QChar ch = line[i];
        if (ch == ' ') {
            result += " ";
            continue;
        }
        if (ch == '-') {
            result += "<span style='color: #666666;'>-</span>";
            continue;
        }
        if (ch == '@') {
            continue;
        }
        if (ch == '#') {
            if (charIndex == targetCharInLine) {
                result += "<span style='color: #555555;'>&nbsp;</span>";
            }
            charIndex++;
            continue;
        }

        bool isPlaceholder = (ch == '~');
        bool isCurrent = (charIndex == targetCharInLine);
        QString color;

        if (lineHasKorean) {
            // [Korean Line] standard char-by-char highlighting (1:1 preservation)
            if (charIndex < targetCharInLine) {
                color = "#888888"; // Past
            } else if (isCurrent) {
                color = "#FFFF00"; // Current
            } else {
                color = "#FFFFFF"; // Future
            }
        } else {
            // [English Line] Hybrid word/character highlighting.
            int currentWordIdx = -1;
            for (int w = 0; w < wordStartIndices.size(); ++w) {
                if (charIndex >= wordStartIndices[w] && charIndex <= wordEndIndices[w]) {
                    currentWordIdx = w;
                    break;
                }
            }
            
            int targetWordIdx = -1;
            for (int w = 0; w < wordStartIndices.size(); ++w) {
                if (targetCharInLine >= wordStartIndices[w] && targetCharInLine <= wordEndIndices[w]) {
                    targetWordIdx = w;
                    break;
                }
            }
            
            if (currentWordIdx != -1 && targetWordIdx != -1) {
                if (currentWordIdx < targetWordIdx) {
                    color = "#888888"; // Past word
                } else if (currentWordIdx == targetWordIdx) {
                    // Active word: color the word golden yellow, but gray out past characters inside it
                    if (charIndex < targetCharInLine) {
                        color = "#888888"; // Past character in active word
                    } else if (isCurrent) {
                        color = "#FFFF00"; // Current active character (Bright Yellow)
                    } else {
                        color = "#FFDD00"; // Remaining characters in active word (Golden Yellow)
                    }
                } else {
                    color = "#FFFFFF"; // Future word
                }
            } else {
                // Fallback
                if (charIndex < targetCharInLine) {
                    color = "#888888";
                } else if (isCurrent) {
                    color = "#FFFF00";
                } else {
                    color = "#FFFFFF";
                }
            }
        }

        if (isPlaceholder && !isCurrent) {
            color = (charIndex < targetCharInLine) ? "#555555" : "#666666";
        }
        QString displayChar = isPlaceholder ? QStringLiteral("~") : QString(ch);
        if (isCurrent) {
            result += QString("<span style='color: %1; font-weight: bold; font-size: 18px;'>%2</span>").arg(color).arg(displayChar);
        } else {
            result += QString("<span style='color: %1;'>%2</span>").arg(color).arg(displayChar);
        }
        charIndex++;  // Advance beat index for rendered lyric characters
    }

    if (result.isEmpty()) {
        result = sanitizeForDisplay(line);
    }

    lyricLabels[targetLine]->setText(result);
}

void LyricsWindow::setSyllableProgressF(double prog)
{
    if (allLyrics.isEmpty()) return;
    if (prog < 0.0) prog = 0.0;
    int m = (int)prog;                 // floor (prog >= 0)
    double frac = prog - (double)m;

    // Render the current line discretely only when the integer index changes
    // (keeps the existing in-line look and avoids re-rendering it every tick).
    if (m != m_smoothLastM) {
        setSyllableProgress(m);
        m_smoothLastM = m;
    }

    // Map m -> (line, index-in-line) to detect a line boundary.
    int eff = m;
    if (m_hasRepeatToStart && m_totalUnitsPerCycle > 0)
        eff = m % m_totalUnitsPerCycle;

    int line = -1, idxInLine = 0, remaining = eff;
    for (int L = 0; L < allLyrics.size(); ++L) {
        int u = (L < m_lineUnitCounts.size()) ? m_lineUnitCounts[L] : countUnitsInLine(allLyrics[L]);
        if (u <= 0) continue;
        if (remaining < u) { line = L; idxInLine = remaining; break; }
        remaining -= u;
    }
    if (line < 0) return;

    int lineUnits = (line < m_lineUnitCounts.size()) ? m_lineUnitCounts[line] : countUnitsInLine(allLyrics[line]);
    if (idxInLine != lineUnits - 1) return;   // not at a line boundary -> nothing to fade

    // At the last syllable of the line: fade the NEXT line's first syllable in by
    // frac so the highlight glides across the inter-line (instrumental) gap instead
    // of pausing then snapping.
    int nextLine = -1;
    for (int L = line + 1; L < allLyrics.size(); ++L) {
        int u = (L < m_lineUnitCounts.size()) ? m_lineUnitCounts[L] : countUnitsInLine(allLyrics[L]);
        if (u > 0) { nextLine = L; break; }
    }
    if (nextLine >= 0 && frac > 0.0)
        renderFirstSyllableFade(nextLine, frac);
}

void LyricsWindow::renderFirstSyllableFade(int lineIdx, double frac)
{
    if (lineIdx < 0 || lineIdx >= lyricLabels.size() || lineIdx >= allLyrics.size()) return;
    frac = qBound(0.0, frac, 1.0);
    int blue = (int)qRound(255.0 * (1.0 - frac));        // white(#FFFFFF) -> yellow(#FFFF00)
    QString fadeColor = QString("#FFFF%1").arg(blue, 2, 16, QChar('0')).toUpper();

    const QString& line = allLyrics[lineIdx];
    QString result;
    bool firstUnitColored = false;
    for (int i = 0; i < line.size(); ++i) {
        QChar ch = line[i];
        if (ch == ' ') { result += " "; continue; }
        if (ch == '-') { result += "<span style='color:#666666;'>-</span>"; continue; }
        if (ch == '@' || ch == '#') { continue; }
        QChar disp = (ch == '~') ? QChar('~') : ch;
        if (!firstUnitColored) {
            result += QString("<span style='color:%1;'>%2</span>").arg(fadeColor).arg(disp);
            firstUnitColored = true;
        } else {
            result += QString("<span style='color:#FFFFFF;'>%1</span>").arg(disp);
        }
    }
    if (result.isEmpty()) result = sanitizeForDisplay(line);
    lyricLabels[lineIdx]->setText(result);
}

void LyricsWindow::setIssHighlight(int lineIndex, const QVector<bool>& highlightMask)
{
    if (lineIndex < 0 || lineIndex >= lyricLabels.size())
        return;

    if (lineIndex != currentLineIndex) {
        setCurrentLine(lineIndex);
    }

    QString line = allLyrics[lineIndex];
    QString result;
    result.reserve(line.size() * 10);

    for (int i = 0; i < line.size(); ++i) {
        QChar ch = line[i];

        if (ch == ' ') {
            result += " ";
            continue;
        }

        bool isHighlighted = (i < highlightMask.size() && highlightMask[i]);
        QString color = isHighlighted ? "#FFFF00" : "#FFFFFF";

        if (isHighlighted) {
            result += QString("<span style='color: %1; font-weight: bold; font-size: 18px;'>%2</span>")
                      .arg(color).arg(ch);
        } else {
            result += QString("<span style='color: %1;'>%2</span>")
                      .arg(color).arg(ch);
        }
    }

    if (result.isEmpty()) {
        result = sanitizeForDisplay(line);
    }

    lyricLabels[lineIndex]->setText(result);
}

void LyricsWindow::reset()
{
    currentLineIndex = -1;
    currentProgress = 0.0;
    // 모든 ?�벨 초기 ?��??�로 복원
    for (int i = 0; i < lyricLabels.size(); ++i)
    {
        lyricLabels[i]->setStyleSheet(normalLineStyle);
        QString fallback = (i < m_displayLyrics.size()) ? m_displayLyrics[i] : sanitizeForDisplay(allLyrics[i]);
        lyricLabels[i]->setText(fallback);
    }
    // ?�크롤을 �??�로
    scrollArea->verticalScrollBar()->setValue(0);
}
void LyricsWindow::updateDisplay()
{
    if (currentLineIndex < 0 || currentLineIndex >= lyricLabels.size())
        return;
    QString processedLine = processLyricLine(allLyrics[currentLineIndex], currentLineIndex, currentProgress);
    lyricLabels[currentLineIndex]->setText(processedLine);
}
QString LyricsWindow::processLyricLine(const QString& line, int lineIndex, double progress)
{
    Q_UNUSED(lineIndex);
    // ?�이?�으�??�절 분리
    QString cleaned = sanitizeForDisplay(line);
    QStringList parts = cleaned.split('-', Qt::KeepEmptyParts);
    if (parts.size() <= 1)
    {
        // ?�이?�이 ?�으�?그냥 반환
        return cleaned;
    }
    // 진행률에 ?�라 ?�절�??�상 ?�용
    QString result;
    int totalParts = parts.size();
    for (int i = 0; i < totalParts; ++i)
    {
        double partStart = (double)i / totalParts;
        double partEnd = (double)(i + 1) / totalParts;
        QString part = parts[i];
        if (progress >= partEnd)
        {
            // 지?�간 ?�절 - ?�색
            result += QString("<span style='color: #888888;'>%1</span>").arg(part);
        }
        else if (progress >= partStart && progress < partEnd)
        {
            // ?�재 ?�절 - ?��???+ 굵게
            result += QString("<span style='color: #FFFF00; font-weight: bold;'>%1</span>").arg(part);
        }
        else
        {
            // ?�직 ??지?�간 ?�절 - ?�색
            result += QString("<span style='color: #FFFFFF;'>%1</span>").arg(part);
        }
        // ?�이??추�? (마�?�??�외)
        if (i < totalParts - 1)
        {
            result += "<span style='color: #888888;'>-</span>";
        }
    }
    return result;
}
void LyricsWindow::setTitle(const QString& title)
{
    titleLabel->setText(QStringLiteral(u"\u266B %1").arg(title));
}

void LyricsWindow::setCurrentFilePath(const QString& filePath)
{
    m_currentFilePath = filePath;
}

QString LyricsWindow::externalLyricsFilePath() const
{
    if (m_currentFilePath.isEmpty()) {
        return QString();
    }

    QFileInfo info(m_currentFilePath);
    if (info.suffix().compare(QStringLiteral("nob"), Qt::CaseInsensitive) != 0) {
        return QString();
    }

    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".txt");
}

bool LyricsWindow::writeExternalLyrics(const QStringList& lyrics)
{
    if (m_currentFilePath.isEmpty()) {
        return false;
    }

    return NobFileHandler::saveExternalLyrics(m_currentFilePath, lyrics);
}

bool LyricsWindow::exportLyricsToFile(const QStringList& lyrics, QWidget *parent)
{
    QString defaultPath = externalLyricsFilePath();
    if (defaultPath.isEmpty()) {
        if (!m_currentFilePath.isEmpty()) {
            QFileInfo info(m_currentFilePath);
            defaultPath = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".txt");
        } else {
            defaultPath = QDir::homePath() + QLatin1String("/lyrics.txt");
        }
    }

    const QString filter = tr("Text Files (*.txt);;All Files (*.*)");
    QString targetPath = QFileDialog::getSaveFileName(parent, tr("Export Lyrics"), defaultPath, filter);
    if (targetPath.isEmpty()) {
        return false;
    }

    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(parent, tr("Export Lyrics"),
                             tr("Failed to save lyrics to %1.")
                                 .arg(QDir::toNativeSeparators(targetPath)));
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
        QMessageBox::warning(parent, tr("Export Lyrics"),
                             tr("Failed to save lyrics to %1.")
                                 .arg(QDir::toNativeSeparators(targetPath)));
        return false;
    }

    QMessageBox::information(parent, tr("Export Lyrics"),
                             tr("Lyrics saved to %1.")
                                 .arg(QDir::toNativeSeparators(targetPath)));
    return true;
}
void LyricsWindow::positionBesideMainWindow()
{
    if (parentMainWindow) {
        QRect parentFrameGeometry = parentMainWindow->frameGeometry();
        QRect parentClientGeometry = parentMainWindow->geometry();
        QScreen *mainWindowScreen = QApplication::screenAt(parentFrameGeometry.center());
        if (!mainWindowScreen) {
            mainWindowScreen = QApplication::primaryScreen();
        }
        QRect screenGeometry = mainWindowScreen->availableGeometry();
        // Calculate same dimensions as ChannelMonitor
        int channelHeight = 24;
        int headerHeight = 35;
        int confidenceHeight = 45;
        int layoutMargins = 16;
        int layoutSpacing = 38;
        int totalHeight = headerHeight + confidenceHeight + (16 * channelHeight) + layoutMargins + layoutSpacing;
        // Reduced width for lyrics window
        int totalWidth = 400;
        // Position to the RIGHT of main window (opposite of channel monitor)
        int x = parentFrameGeometry.x() + parentFrameGeometry.width();
        int y = parentFrameGeometry.y();
        // Check if there's enough space on the right
        if (x + totalWidth > screenGeometry.right()) {
            // Not enough space on right, clamp to screen edge
            x = screenGeometry.right() - totalWidth;
        }
        // Ensure within screen bounds
        if (x < screenGeometry.left()) {
            x = screenGeometry.left();
        }
        if (y < screenGeometry.top()) {
            y = screenGeometry.top();
        }
        if (y + totalHeight > screenGeometry.bottom()) {
            y = screenGeometry.bottom() - totalHeight;
        }
        move(x, y);
        setFixedSize(totalWidth, totalHeight);
    }
}
void LyricsWindow::closeEvent(QCloseEvent *event)
{
    emit closed();
    event->accept();
}
// NOB 채널 ?택 관???수??
void LyricsWindow::setNobFile(bool isNob)
{
    m_isNobFile = isNob;

    // The manual channel picker is gone from the UI (2026-07-31). Detection now
    // scores every channel by note count AND by how late it starts, then trims
    // the intro off a guide that plays from the top of the song, so choosing by
    // hand no longer helps - and picking the wrong channel silently broke sync.
    // The combo is still populated so the rest of the code keeps working.
    m_channelWidget->setVisible(false);
    if (isNob) {
        m_channelCombo->clear();
        for (int channel = 1; channel <= 16; ++channel) {
            m_channelCombo->addItem(QString("Channel %1").arg(channel), channel);
        }

        updateChannelSelection();
    }
}
void LyricsWindow::setChannelWidgetVisible(bool)
{
    // Deliberately ignored - see setNobFile().
    if (m_channelWidget) {
        m_channelWidget->setVisible(false);
    }
}
void LyricsWindow::setCurrentChannel(int channel)
{
    if (channel < 0 || channel > 16) {
        qWarning() << "[LyricsWindow] Invalid channel:" << channel << "(must be 0~16)";
        return;
    }
    if (m_currentChannel != channel) {
        m_currentChannel = channel;
        qDebug() << "[LyricsWindow] Channel set to:" << m_currentChannel;
    }
    updateChannelSelection();
}
void LyricsWindow::updateChannelSelection()
{
    if (!m_channelCombo) {
        return;
    }
    int index = m_channelCombo->findData(m_currentChannel);
    if (index < 0 && m_channelCombo->count() > 0) {
        index = 0;
        m_currentChannel = m_channelCombo->itemData(index).toInt();
    }
    if (index >= 0) {
        QSignalBlocker blocker(m_channelCombo);
        m_channelCombo->setCurrentIndex(index);
    }
}
void LyricsWindow::onChannelSelectionChanged(int index)
{
    if (!m_channelCombo || index < 0) {
        return;
    }
    int newChannel = m_channelCombo->itemData(index).toInt();
    if (newChannel <= 0) {
        return;
    }
    if (newChannel != m_currentChannel) {
        m_currentChannel = newChannel;
        qDebug() << "[LyricsWindow] User changed channel to:" << newChannel;
        emit channelChanged(newChannel);
    }
}
