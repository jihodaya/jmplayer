#include "channelmonitor.h"
#include "constants.h"
#include <QApplication>
#include <QCloseEvent>
#include <QScreen>
#include <QListWidget>
#include <QPainter>
#include <QTimer>
#include <QScrollArea>


class ImsLevelMeterWidget : public QWidget {
public:
    explicit ImsLevelMeterWidget(QWidget *parent = nullptr) : QWidget(parent) {
        m_decayTimer = new QTimer(this);
        connect(m_decayTimer, &QTimer::timeout, this, &ImsLevelMeterWidget::decayVolumes);
        m_decayTimer->start(40);
    }

    void setVolumes(const QList<int>& volumes) {
        if (m_decayedVolumes.size() != volumes.size()) {
            m_decayedVolumes.resize(volumes.size(), 0);
            m_peakVolumes.resize(volumes.size(), 0);
            m_peakTimers.resize(volumes.size(), 0);
        }
        for (int i = 0; i < volumes.size(); ++i) {
            if (volumes[i] > m_decayedVolumes[i]) {
                m_decayedVolumes[i] = volumes[i];
            }
            if (volumes[i] > m_peakVolumes[i]) {
                m_peakVolumes[i] = volumes[i];
                m_peakTimers[i] = 12;
            }
        }
        update();
    }

    void setInstruments(const QStringList& insts) {
        m_instruments = insts;
        
        // 보이스 홀드 타이머 리사이징
        if (m_voiceActiveHold.size() != insts.size()) {
            m_voiceActiveHold.resize(insts.size(), 0);
        }
        
        // KeyOn 상태가 감지되면 홀드 카운터 설정 (6프레임 = 약 240ms 동안 활성 상태 유지)
        for (int i = 0; i < insts.size(); ++i) {
            QString raw = insts[i];
            QStringList tokens = raw.split('|');
            bool isOn = tokens.value(3, "0") == "1";
            if (isOn) {
                m_voiceActiveHold[i] = 6; 
            }
        }
        
        update();
    }

private:
    void decayVolumes() {
        bool changed = false;
        for (int i = 0; i < m_decayedVolumes.size(); ++i) {
            if (m_decayedVolumes[i] > 0) {
                m_decayedVolumes[i] -= 4; 
                if (m_decayedVolumes[i] < 0) m_decayedVolumes[i] = 0;
                changed = true;
            }
            if (m_peakTimers[i] > 0) {
                m_peakTimers[i]--;
            } else if (m_peakVolumes[i] > 0) {
                m_peakVolumes[i] -= 2; 
                if (m_peakVolumes[i] < 0) m_peakVolumes[i] = 0;
                changed = true;
            }
        }
        
        // 보이스 활성화 홀드 타이머 감쇠
        for (int i = 0; i < m_voiceActiveHold.size(); ++i) {
            if (m_voiceActiveHold[i] > 0) {
                m_voiceActiveHold[i]--;
                changed = true;
            }
        }
        
        if (changed) update();
    }

    // 기존의 이쁜 그라데이션 색상 보간 함수
    QColor getGradualColor(float ratio) {
        struct ColorStop { float pos; QColor color; };
        static const ColorStop stops[] = {
            {0.00f, QColor(150, 240, 255)}, 
            {0.25f, QColor(0, 160, 255)},   
            {0.55f, QColor(0, 255, 60)},    
            {0.75f, QColor(255, 255, 0)},   
            {0.90f, QColor(255, 120, 0)},   
            {1.00f, QColor(255, 0, 0)}      
        };
        for (int i = 0; i < 5; ++i) {
            if (ratio >= stops[i].pos && ratio <= stops[i+1].pos) {
                float f = (ratio - stops[i].pos) / (stops[i+1].pos - stops[i].pos);
                int r = (int)(stops[i].color.red() * (1.0f - f) + stops[i+1].color.red() * f);
                int g = (int)(stops[i].color.green() * (1.0f - f) + stops[i+1].color.green() * f);
                int b = (int)(stops[i].color.blue() * (1.0f - f) + stops[i+1].color.blue() * f);
                return QColor(r, g, b);
            }
        }
        return stops[5].color;
    }

    QList<int> m_decayedVolumes;
    QList<int> m_peakVolumes;
    QList<int> m_peakTimers;
    QStringList m_instruments;
    QList<int> m_voiceActiveHold; // 보이스 활성 유지 타이머 리스트
    QTimer *m_decayTimer;

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        int w = width();
        int h = height();

        // 전체 배경
        painter.fillRect(0, 0, w, h, QColor(12, 12, 15));

        int numVoices = 20;
        int marginX = 6;
        int marginY = 6;
        int rowSpacing = 2;
        
        int availableH = h - (marginY * 2);
        int rowH = (availableH - (rowSpacing * (numVoices - 1))) / numVoices;
        if (rowH < 12) rowH = 12;

        // 기존 473px 폭에 맞춘 콤팩트한 좌우 분할 X 좌표
        int leftAreaW = 180;

        // 1. 중간 영역 경계 세로선 그리기 (입체감 부여)
        painter.setPen(QColor(40, 40, 50));
        painter.drawLine(leftAreaW, marginY, leftAreaW, h - marginY);
        painter.setPen(QColor(20, 20, 25));
        painter.drawLine(leftAreaW + 1, marginY, leftAreaW + 1, h - marginY);

        // 폰트 크기 동적 지정 (겹침 방지)
        int fontSize = (rowH >= 15) ? 8 : 7;
        int fontCardSize = (rowH >= 15) ? 8 : 7;

        for (int i = 0; i < numVoices; ++i) {
            int y = marginY + i * (rowH + rowSpacing);
            
            // 실시간 레벨 데이터
            int vol = (i < m_decayedVolumes.size()) ? m_decayedVolumes[i] : 0;
            int peak = (i < m_peakVolumes.size()) ? m_peakVolumes[i] : 0;

            // 실시간 물리 상태 데이터 파싱
            QString raw = (i < m_instruments.size()) ? m_instruments[i] : "--|   |  0|0";
            QStringList tokens = raw.split('|');
            QString instName = tokens.value(0, "--");
            if (instName.isEmpty()) instName = "--";
            if (instName.length() > 8) instName = instName.left(8); // 카드 크기에 맞춰 생략
            QString noteStr = tokens.value(1, "   ");
            QString volStr = tokens.value(2, "  0");
            bool isOn = tokens.value(3, "0") == "1";
            bool isVisualOn = isOn || (i < m_voiceActiveHold.size() && m_voiceActiveHold[i] > 0);

            bool isMissing = false;
            bool isSubstituted = false;
            bool isPlaceholder = instName == "--";

            if (instName.startsWith('~')) {
                isSubstituted = true;
                instName = instName.mid(1);
            } else if (instName.startsWith('*')) {
                isMissing = true;
                instName = instName.mid(1);
            }

            // ----------------------------------------------------
            // [좌측 영역]: 물리 보이스 정보 (01-20, 악기명, 음계, 벨로시티)
            // ----------------------------------------------------
            
            // 왼쪽 얇은 데코 바 (차분한 청록색으로 대비 완화)
            painter.setPen(Qt::NoPen);
            if (isVisualOn) {
                painter.setBrush(QColor(0, 140, 160));
            } else {
                painter.setBrush(QColor(42, 45, 48));
            }
            painter.drawRect(marginX, y + 2, 2, rowH - 4);

            QString voiceLabel = QString("%1").arg(i + 1, 2, 10, QChar('0'));
            QFont labelFont("Consolas");
            labelFont.setPointSize(fontSize);
            labelFont.setBold(true);
            painter.setFont(labelFont);
            if (isVisualOn) {
                painter.setPen(QColor(115, 175, 190)); 
            } else {
                painter.setPen(QColor(80, 85, 90)); 
            }
            painter.drawText(marginX + 6, y, 18, rowH, Qt::AlignVCenter | Qt::AlignLeft, voiceLabel);

            // 악기명 카드 그리기
            int cardX = marginX + 24;
            int cardW = 72; 
            
            painter.setPen(Qt::NoPen);
            if (isPlaceholder) {
                painter.setBrush(QColor(20, 20, 22, 100));
            } else if (isVisualOn) {
                if (isSubstituted) {
                    painter.setBrush(QColor(90, 42, 15, 180)); 
                } else if (isMissing) {
                    painter.setBrush(QColor(90, 20, 20, 180)); 
                } else {
                    painter.setBrush(QColor(45, 45, 52, 220)); 
                }
            } else {
                painter.setBrush(QColor(28, 28, 32, 120));
            }
            painter.drawRoundedRect(cardX, y + 1, cardW, rowH - 2, 3, 3);

            // 악기 텍스트 출력
            QFont instFont("Malgun Gothic");
            instFont.setPointSize(fontCardSize);
            instFont.setBold(isVisualOn);
            painter.setFont(instFont);

            if (isPlaceholder) {
                painter.setPen(QColor(55, 55, 60, 150));
                painter.drawText(cardX, y, cardW, rowH, Qt::AlignCenter, "---");
            } else {
                if (isVisualOn) {
                    if (isSubstituted) {
                        painter.setPen(QColor(220, 145, 105)); 
                        painter.drawText(cardX, y, cardW, rowH, Qt::AlignCenter, QString("≈%1").arg(instName));
                    } else if (isMissing) {
                        painter.setPen(QColor(220, 105, 105)); 
                        painter.drawText(cardX, y, cardW, rowH, Qt::AlignCenter, QString("!%1").arg(instName));
                    } else {
                        painter.setPen(QColor(215, 215, 220)); 
                        painter.drawText(cardX, y, cardW, rowH, Qt::AlignCenter, instName);
                    }
                } else {
                    painter.setPen(QColor(90, 90, 98)); 
                    QString dispName = instName;
                    if (isSubstituted) dispName = "≈" + instName;
                    else if (isMissing) dispName = "!" + instName;
                    painter.drawText(cardX, y, cardW, rowH, Qt::AlignCenter, dispName);
                }
            }

            // 음계 (Note) & 벨로시티
            int detailsX = cardX + cardW + 4;
            
            // 음계 (Note)
            QFont noteFont("Consolas");
            noteFont.setPointSize(fontSize);
            noteFont.setBold(isVisualOn);
            painter.setFont(noteFont);
            if (isVisualOn && !noteStr.trimmed().isEmpty()) {
                painter.setPen(QColor(120, 195, 130)); 
                painter.drawText(detailsX, y, 26, rowH, Qt::AlignVCenter | Qt::AlignLeft, noteStr);
            } else {
                painter.setPen(QColor(50, 50, 55));
                painter.drawText(detailsX, y, 26, rowH, Qt::AlignVCenter | Qt::AlignLeft, noteStr.trimmed().isEmpty() ? "..." : noteStr);
            }

            // 벨로시티 (Velocity)
            if (isVisualOn) {
                painter.setPen(QColor(220, 195, 110)); 
                painter.drawText(detailsX + 28, y, 20, rowH, Qt::AlignVCenter | Qt::AlignLeft, volStr);
            } else {
                painter.setPen(QColor(50, 50, 55));
                painter.drawText(detailsX + 28, y, 20, rowH, Qt::AlignVCenter | Qt::AlignLeft, volStr);
            }

            // ----------------------------------------------------
            // [우측 영역]: OPL FM 채널 레벨 (가로형 LED VU 미터)
            // ----------------------------------------------------
            
            int rightX = leftAreaW + 6;

            // 채널 번호/드럼 레이블 표시 (01-20)
            QString chanLabelStr;
            if (i >= 0 && i <= 5) {
                chanLabelStr = QString("%1").arg(i + 1, 2, 10, QChar('0'));
            } else if (i >= 6 && i <= 10) {
                static const char* drumLabels[] = { "BD", "SD", "TM", "CY", "HH" };
                chanLabelStr = drumLabels[i - 6];
            } else {
                chanLabelStr = QString("%1").arg(i + 1, 2, 10, QChar('0'));
            }

            painter.setFont(labelFont);
            if (isVisualOn) {
                painter.setPen(QColor(115, 135, 155)); 
            } else {
                painter.setPen(QColor(55, 60, 65));
            }
            painter.drawText(rightX, y, 18, rowH, Qt::AlignVCenter | Qt::AlignLeft, chanLabelStr);

            // 가로형 LED VU 미터 그리기
            int barX = rightX + 20;
            int barW = w - barX - marginX;
            if (barW < 20) barW = 20;

            int numSegments = 32;
            float segSpacing = 1.5f;
            float totalSpacing = segSpacing * (numSegments - 1);
            float segW = (barW - totalSpacing) / numSegments;
            if (segW < 1.0f) segW = 1.0f;

            int activeSegments = (vol * numSegments) / 127;
            int peakSegment = (peak * numSegments) / 127;

            for (int s = 0; s < numSegments; ++s) {
                float segX = barX + s * (segW + segSpacing);
                float ratio = (float)s / numSegments;
                
                QColor segColor = getGradualColor(ratio);

                if (s < activeSegments) {
                    painter.fillRect(QRectF(segX, y + 2, segW, rowH - 4), segColor);
                } else {
                    painter.fillRect(QRectF(segX, y + 2, segW, rowH - 4), QColor(25, 25, 28));
                }

                if (s == peakSegment && s > 0) {
                    painter.fillRect(QRectF(segX, y + 2, segW, rowH - 4), QColor(255, 255, 255, 230));
                }
            }
        }
    }
};;

ChannelWidget::ChannelWidget(int channelNumber, QWidget *parent)
    : QFrame(parent), channel(channelNumber), currentVolume(0), currentProgram(0), hasProgramChangeReceived(false), isChannelActive(false)
{
    setupUI();

    // Pre-configure drum channel (channel 10, index 9)
    if (channel == 9) {
        hasProgramChangeReceived = true; // Drum channel always has "program"
        currentProgram = 0; // Standard drum kit
    }

    decayTimer = new QTimer(this);
    decayTimer->setSingleShot(true);
    decayTimer->setInterval(100);
    connect(decayTimer, &QTimer::timeout, [this]() {
        if (currentVolume > 0) {
            currentVolume = qMax(0, currentVolume - 10);
            volumeBar->setValue(currentVolume);
            if (currentVolume > 0) {
                decayTimer->start();
            }
        }
    });

    // Timer to automatically hide channel info after inactivity
    inactivityTimer = new QTimer(this);
    inactivityTimer->setSingleShot(true);
    inactivityTimer->setInterval(3000); // 3 seconds of inactivity
    connect(inactivityTimer, &QTimer::timeout, this, &ChannelWidget::setInactive);

    // Start in inactive state
    setInactive();
}

void ChannelWidget::setupUI()
{
    setFrameStyle(QFrame::Box);
    setLineWidth(1);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);

    // Channel number
    channelLabel = new QLabel(QString::number(channel + 1), this);
    channelLabel->setFixedWidth(UI::ChannelMonitor::CHANNEL_LABEL_WIDTH);
    channelLabel->setAlignment(Qt::AlignCenter);
    channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_DEFAULT);

    // Volume bar
    volumeBar = new QProgressBar(this);
    volumeBar->setRange(0, 127);
    volumeBar->setValue(0);
    volumeBar->setFixedWidth(UI::ChannelMonitor::VOLUME_BAR_WIDTH);
    volumeBar->setFixedHeight(UI::ChannelMonitor::VOLUME_BAR_HEIGHT);
    volumeBar->setTextVisible(false);

    // Program number with instrument name
    programLabel = new QLabel("", this);
    programLabel->setFixedWidth(UI::ChannelMonitor::PROGRAM_LABEL_WIDTH);
    programLabel->setAlignment(Qt::AlignCenter);
    programLabel->setStyleSheet(Styles::ChannelMonitor::PROGRAM_LABEL_DEFAULT);

    // Instrument name
    instrumentLabel = new QLabel("", this);
    instrumentLabel->setFixedWidth(UI::ChannelMonitor::INSTRUMENT_LABEL_WIDTH);
    instrumentLabel->setAlignment(Qt::AlignLeft);
    instrumentLabel->setStyleSheet(Styles::ChannelMonitor::INSTRUMENT_LABEL_DEFAULT);

    // Active notes
    notesLabel = new QLabel("", this);
    notesLabel->setMinimumWidth(UI::ChannelMonitor::NOTES_LABEL_WIDTH);
    notesLabel->setStyleSheet(Styles::ChannelMonitor::NOTES_LABEL_DEFAULT);
    notesLabel->setTextFormat(Qt::RichText); // Enable HTML formatting

    layout->addWidget(channelLabel);
    layout->addWidget(volumeBar);
    layout->addWidget(programLabel);
    layout->addWidget(instrumentLabel);
    layout->addWidget(notesLabel);
    // Remove addStretch() to eliminate right-side empty space

    setFixedHeight(UI::ChannelMonitor::CHANNEL_WIDGET_HEIGHT);
}

void ChannelWidget::setNote(int note, int velocity)
{
    setActive(); // Activate channel when note is played

    QString noteName = QString("C C#D D#E F F#G G#A A#B ").mid((note % 12) * 2, 2).trimmed();
    int octave = (note / 12) - 1;
    QString noteStr = QString("%1%2").arg(noteName).arg(octave);

    if (!activeNotes.contains(noteStr)) {
        activeNotes.append(noteStr);

        // Color-code notes based on velocity
        QString coloredText;
        for (const QString &note : activeNotes) {
            if (note == noteStr) {
                // New note with velocity-based color
                if (velocity > 100) {
                    coloredText += QString("<span style='color: #FF4444; font-weight: bold;'>%1</span> ").arg(note);
                } else if (velocity > 64) {
                    coloredText += QString("<span style='color: #FFFF44; font-weight: bold;'>%1</span> ").arg(note);
                } else {
                    coloredText += QString("<span style='color: #44FF44;'>%1</span> ").arg(note);
                }
            } else {
                coloredText += QString("<span style='color: #888888;'>%1</span> ").arg(note);
            }
        }
        notesLabel->setText(coloredText);
    }

    // Animate volume bar
    currentVolume = qMax(currentVolume, velocity);
    volumeBar->setValue(currentVolume);

    // Flash effect for channel label
    if (velocity > MIDI::HIGH_VELOCITY_THRESHOLD) {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_HIGH);
    } else if (velocity > 40) {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_MEDIUM);
    } else {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_LOW);
    }

    // Reset channel label color after brief flash
    QTimer::singleShot(100, [this]() {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_DEFAULT);
    });

    decayTimer->start();
}

void ChannelWidget::clearNote(int note)
{
    QString noteName = QString("C C#D D#E F F#G G#A A#B ").mid((note % 12) * 2, 2).trimmed();
    int octave = (note / 12) - 1;
    QString noteStr = QString("%1%2").arg(noteName).arg(octave);

    activeNotes.removeAll(noteStr);

    // Update display with remaining notes
    QString coloredText;
    for (const QString &remainingNote : activeNotes) {
        coloredText += QString("<span style='color: #888888;'>%1</span> ").arg(remainingNote);
    }
    notesLabel->setText(coloredText);
}

void ChannelWidget::setController(int controller, int value)
{
    setActive(); // Activate channel when controller is changed

    if (controller == 7) { // Volume controller
        currentVolume = value;
        volumeBar->setValue(currentVolume);
    }
}

void ChannelWidget::setProgram(int program)
{
    currentProgram = program; // Store current program
    hasProgramChangeReceived = true; // Mark that we've received a program change
    setActive(); // Activate channel when program is changed

    // Always update program and instrument display (even if already active)
    programLabel->setText(QString::number(program + 1));
    QString instrumentName = getInstrumentName(program);
    instrumentLabel->setText(instrumentName);

    // Flash effect for program change
    programLabel->setStyleSheet(Styles::ChannelMonitor::PROGRAM_ACTIVE);
    instrumentLabel->setStyleSheet(Styles::ChannelMonitor::INSTRUMENT_ACTIVE);

    // Reset program label color after brief flash
    QTimer::singleShot(UI::ChannelMonitor::DECAY_TIMER_INTERVAL, [this]() {
        programLabel->setStyleSheet(Styles::ChannelMonitor::PROGRAM_LABEL_DEFAULT);
        instrumentLabel->setStyleSheet(Styles::ChannelMonitor::INSTRUMENT_LABEL_DEFAULT);
    });
}

void ChannelWidget::setLevel(int volume)
{
    // IMS용: 노트 목록 변경 없이 볼륨바와 채널 플래시만 업데이트
    isChannelActive = true;
    currentVolume = volume;
    volumeBar->setValue(volume);

    if (volume > UI::ChannelMonitor::VOLUME_BAR_WIDTH) { // 강한 소리
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_HIGH);
    } else if (volume > 40) {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_MEDIUM);
    } else if (volume > 0) {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_LOW);
    } else {
        channelLabel->setStyleSheet(Styles::ChannelMonitor::CHANNEL_LABEL_DEFAULT);
    }

    decayTimer->start();
    inactivityTimer->start();
}

void ChannelWidget::setImsChannelMode(const QString& instrumentName)
{
    // IMS 전용: 채널번호 + 레벨바 + 악기명만 표시
    // programLabel, notesLabel 숨기기
    programLabel->hide();
    notesLabel->hide();
    // instrumentLabel이 남은 공간을 더 사용하도록
    instrumentLabel->setFixedWidth(UI::ChannelMonitor::INSTRUMENT_LABEL_WIDTH
                                   + UI::ChannelMonitor::PROGRAM_LABEL_WIDTH
                                   + UI::ChannelMonitor::NOTES_LABEL_WIDTH);
    instrumentLabel->setText(instrumentName);
    instrumentLabel->setStyleSheet("color: #FFAA00; font-size: 10px; font-family: monospace;");
    // hasProgramChangeReceived 막아두기 (setActive가 instrumentLabel 덮어쓰지 않도록)
    hasProgramChangeReceived = false;
    currentProgram = -1;
}

void ChannelWidget::reset()
{
    activeNotes.clear();
    notesLabel->setText("");
    currentVolume = 0;
    volumeBar->setValue(0);
    setInactive();
}

void ChannelWidget::setInactive()
{
    isChannelActive = false;
    // Don't reset hasProgramChangeReceived - keep program change info
    // Don't reset currentProgram - keep the last known program
    programLabel->setText("");
    instrumentLabel->setText("");
    notesLabel->setText("");
    // Keep channel number visible but dim the volume bar
    volumeBar->setValue(0);
    currentVolume = 0;
    activeNotes.clear();
    inactivityTimer->stop();
}

void ChannelWidget::setActive()
{
    isChannelActive = true;

    // Only show instrument info if we have program change info or it's a drum channel
    if (channel == 9 || hasProgramChangeReceived) {
        programLabel->setText(QString::number(currentProgram + 1));
        instrumentLabel->setText(getInstrumentName(currentProgram));
    }

    // Reset inactivity timer
    inactivityTimer->start();
}

// Remove duplicate includes

QString ChannelWidget::getInstrumentName(int program)
{
    // Channel 10 (index 9) is always drums in MIDI standard
    if (channel == 9) {
        return "Drum Kit";
    }

    switch (currentSoundMode) {
        case MT32_MODE:
            return getMT32InstrumentName(program);
        case GS_MODE:
        case XG_MODE:
        case GM_MODE:
        default:
            return getGMInstrumentName(program);
    }
}

QString ChannelWidget::getGMInstrumentName(int program)
{
    // General MIDI instrument names (0-127)
    static const QString instruments[] = {
        // Piano (0-7)
        "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        // Chromatic Percussion (8-15)
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        // Organ (16-23)
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        // Guitar (24-31)
        "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
        // Bass (32-39)
        "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        // Strings (40-47)
        "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato", "Orchestral Harp", "Timpani",
        // Ensemble (48-55)
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        // Brass (56-63)
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        // Reed (64-71)
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
        // Pipe (72-79)
        "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        // Synth Lead (80-87)
        "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass+Lead",
        // Synth Pad (88-95)
        "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
        // Synth Effects (96-103)
        "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-Fi",
        // Ethnic (104-111)
        "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        // Percussive (112-119)
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        // Sound effects (120-127)
        "Guitar Fret", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause", "Gunshot"
    };

    if (program >= 0 && program < 128) {
        return instruments[program];
    }
    return "Unknown";
}

QString ChannelWidget::getMT32InstrumentName(int program)
{
    // Roland MT-32 instrument names (0-127)
    static const QString mt32Instruments[] = {
        // Piano (0-7)
        "Acoustic Piano 1", "Acoustic Piano 2", "Acoustic Piano 3", "Electric Piano 1", "Electric Piano 2", "Electric Piano 3", "Electric Piano 4", "Honky-tonk",
        // Chromatic Percussion (8-15)
        "Electric Organ 1", "Electric Organ 2", "Electric Organ 3", "Electric Organ 4", "Pipe Organ 1", "Pipe Organ 2", "Pipe Organ 3", "Accordion",
        // Organ (16-23)
        "Harpsichord 1", "Harpsichord 2", "Harpsichord 3", "Clavi 1", "Clavi 2", "Clavi 3", "Celesta 1", "Celesta 2",
        // Guitar (24-31)
        "Syn Brass 1", "Syn Brass 2", "Syn Brass 3", "Syn Brass 4", "Syn Bass 1", "Syn Bass 2", "Syn Bass 3", "Syn Bass 4",
        // Bass (32-39)
        "Fantasy", "Harmo Pan", "Chorale", "Glasses", "Soundtrack", "Atmosphere", "Warm Bell", "Funny Vox",
        // Strings (40-47)
        "Echo Bell", "Ice Rain", "Oboe 2001", "Echo Pan", "Doctor Solo", "School Daze", "Bellsinger", "Square Wave",
        // Ensemble (48-55)
        "Str Sect 1", "Str Sect 2", "Str Sect 3", "Pizzicato", "Violin 1", "Violin 2", "Cello 1", "Cello 2",
        // Brass (56-63)
        "Contrabass", "Harp 1", "Harp 2", "Guitar 1", "Guitar 2", "Electric Gtr 1", "Electric Gtr 2", "Sitar",
        // Reed (64-71)
        "Acou Bass 1", "Acou Bass 2", "Elec Bass 1", "Elec Bass 2", "Slap Bass 1", "Slap Bass 2", "Fretless 1", "Fretless 2",
        // Pipe (72-79)
        "Flute 1", "Flute 2", "Piccolo 1", "Piccolo 2", "Recorder", "Pan Pipes", "Sax 1", "Sax 2",
        // Synth Lead (80-87)
        "Sax 3", "Sax 4", "Clarinet 1", "Clarinet 2", "Oboe", "English Horn", "Bassoon", "Harmonica",
        // Synth Pad (88-95)
        "Trumpet 1", "Trumpet 2", "Trombone 1", "Trombone 2", "French Horn 1", "French Horn 2", "Tuba", "Brs Sect 1",
        // Synth Effects (96-103)
        "Brs Sect 2", "Vibe 1", "Vibe 2", "Syn Mallet", "Windbell", "Glock", "Tube Bell", "Xylophone",
        // Ethnic (104-111)
        "Marimba", "Koto", "Sho", "Shakuhachi", "Whistle 1", "Whistle 2", "Bottleblow", "Breathpipe",
        // Percussive (112-119)
        "Timpani", "Melodic Tom", "Deep Snare", "Elec Perc 1", "Elec Perc 2", "Taiko", "Taiko Rim", "Cymbal",
        // Sound effects (120-127)
        "Castanets", "Triangle", "Orche Hit", "Telephone", "Bird Tweet", "One Note Jam", "Water Bells", "Jungle Tune"
    };

    if (program >= 0 && program < 128) {
        return mt32Instruments[program];
    }
    return "Unknown";
}

// Initialize static member
ChannelWidget::SoundMode ChannelWidget::currentSoundMode = ChannelWidget::GM_MODE;

// ChannelWidget Implementation
ChannelMonitor::ChannelMonitor(QWidget *parent)
    : QMainWindow(parent), parentMainWindow(parent)
{
    // Ensure same DPI context as parent window
    if (parent) {
        setWindowFlags(windowFlags() | Qt::Tool);  // Inherit DPI from parent
    }

    setupUI();
    positionBesideMainWindow();
}

ChannelMonitor::~ChannelMonitor()
{
}

void ChannelMonitor::setupUI()
{
    setWindowTitle("Channel Monitor");
    QString iconPath = QApplication::applicationDirPath() + "/K_icon.ico";
    QIcon icon(iconPath);
    if (icon.isNull()) {
        // Fallback to resource path
        icon = QIcon(":/K_icon.ico");
    }
    setWindowIcon(icon);

    // Calculate optimal height based on content (16 channels + header + confidence info)
    int channelHeight = 24; // Each channel widget height
    int headerHeight = 35;  // Header label height + padding
    int confidenceHeight = 45; // Height for sound mode + confidence labels (increased for larger text)
    int layoutMargins = 16; // Top and bottom margins (8+8)
    int layoutSpacing = 38; // Space between widgets (slightly increased for larger text)
    int totalHeight = headerHeight + confidenceHeight + (16 * channelHeight) + layoutMargins + layoutSpacing;

    // Calculate optimal width based on content using constants
    int horizontalMargins = 8; // Left and right margins (4+4)
    int horizontalSpacing = 5 * 4; // Space between 5 widgets
    int totalWidth = UI::ChannelMonitor::CHANNEL_LABEL_WIDTH + UI::ChannelMonitor::VOLUME_BAR_WIDTH +
                     UI::ChannelMonitor::PROGRAM_LABEL_WIDTH + UI::ChannelMonitor::INSTRUMENT_LABEL_WIDTH +
                     UI::ChannelMonitor::NOTES_LABEL_WIDTH + horizontalMargins + horizontalSpacing;

    m_fixedWidth = totalWidth;
    m_channelAreaTopHeight = headerHeight + confidenceHeight + layoutMargins + layoutSpacing;
    m_channelHeight = channelHeight;

    setFixedSize(totalWidth, totalHeight);

    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(2);

    // Header
    headerLabel = new QLabel("MIDI Channel Monitor", this);
    headerLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 15px; color: #00e5ff; padding: 6px;");
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mainLayout->addWidget(headerLabel);

    // Sound mode indicator
    soundModeLabel = new QLabel("Mode: GM (General MIDI)", this);
    soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #ffd600; padding: 5px 10px; background-color: #222211; border: 1px solid #ffd600; border-radius: 4px;");
    soundModeLabel->setAlignment(Qt::AlignCenter);
    soundModeLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mainLayout->addWidget(soundModeLabel);

    // Combined confidence score and reliability indicator
    confidenceLabel = new QLabel("Confidence: 25% (Default)", this);
    confidenceLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-size: 12px; color: #cccccc; padding: 4px;");
    confidenceLabel->setAlignment(Qt::AlignCenter);
    confidenceLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    mainLayout->addWidget(confidenceLabel);

    // Initialize reliability info
    currentReliability.confusionHint = "";

    // Container for standard MIDI channels
    midiChannelsContainer = new QWidget(centralWidget);
    QVBoxLayout *midiLayout = new QVBoxLayout(midiChannelsContainer);
    midiLayout->setContentsMargins(0, 0, 0, 0);
    midiLayout->setSpacing(2);

    // Create 16 channel widgets
    for (int i = 0; i < 16; ++i) {
        channelWidgets[i] = new ChannelWidget(i, midiChannelsContainer);
        midiLayout->addWidget(channelWidgets[i]);
    }
    mainLayout->addWidget(midiChannelsContainer);
    
    // Container for IMS channels (no scroll, just a plain widget)
    imsChannelsContainer = new QWidget(centralWidget);
    imsChannelsContainer->setStyleSheet("background-color: transparent;");
    imsChannelsContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout *imsLayout = new QVBoxLayout(imsChannelsContainer);
    imsLayout->setContentsMargins(0, 0, 0, 0);
    imsLayout->setSpacing(2);
    imsChannelsContainer->hide();
    mainLayout->addWidget(imsChannelsContainer);

    // Remove stretch to eliminate empty space at bottom
    // mainLayout->addStretch();

    // Apply dark theme
    setStyleSheet(
        "QMainWindow {"
        "    background-color: #2b2b2b;"
        "    color: #ffffff;"
        "}"
        "QProgressBar {"
        "    border: 1px solid #666666;"
        "    border-radius: 2px;"
        "    background-color: #1a1a1a;"
        "}"
        "QProgressBar::chunk {"
        "    background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #00FF00, stop:0.7 #FFFF00, stop:1 #FF0000);"
        "    border-radius: 1px;"
        "}"
        "QFrame {"
        "    background-color: #3a3a3a;"
        "    border: 1px solid #555555;"
        "}"
    );
}

void ChannelMonitor::positionBesideMainWindow()
{
    if (parentMainWindow) {
        // Use Qt's frame geometry for DPI-aware calculations
        QRect parentFrameGeometry = parentMainWindow->frameGeometry();
        QRect parentClientGeometry = parentMainWindow->geometry();

        // Get the screen that contains the main window
        QScreen *mainWindowScreen = QApplication::screenAt(parentFrameGeometry.center());
        if (!mainWindowScreen) {
            mainWindowScreen = QApplication::primaryScreen();
        }
        QRect screenGeometry = mainWindowScreen->availableGeometry();

        // Force position to the left of main window (no fallback to right)
        int x = parentFrameGeometry.x() - width();

        // Align with main window's top edge (frame top, not client area)
        // This automatically handles DPI scaling through Qt
        int y = parentFrameGeometry.y();

        // If not enough space on left, move to edge of screen but keep on left
        if (x < screenGeometry.x()) {
            x = screenGeometry.x();
        }

        // Ensure the channel monitor doesn't go outside the same screen
        if (x + width() > screenGeometry.right()) {
            x = screenGeometry.right() - width();
        }
        if (x < screenGeometry.left()) {
            x = screenGeometry.left();
        }

        // Keep within screen bounds vertically
        if (y + height() > screenGeometry.bottom()) {
            y = screenGeometry.bottom() - height();
        }
        if (y < screenGeometry.top()) {
            y = screenGeometry.top();
        }

        move(x, y);

        // Apply dark titlebar style to match main window
        #ifdef _WIN32
        if (winId()) {
            HWND hwnd = (HWND)winId();

            // Apply dark mode first
            BOOL darkMode = TRUE;
            ::DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE

            // Also apply newer dark mode attribute if available
            BOOL useImmersiveDarkMode = TRUE;
            ::DwmSetWindowAttribute(hwnd, 19, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1

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
}

void ChannelMonitor::onNoteOn(int channel, int note, int velocity)
{
    if (channel >= 0 && channel < 16) {
        channelWidgets[channel]->setNote(note, velocity);
    }
}

void ChannelMonitor::onNoteOff(int channel, int note)
{
    if (channel >= 0 && channel < 16) {
        channelWidgets[channel]->clearNote(note);
    }
}

void ChannelMonitor::onControllerChange(int channel, int controller, int value)
{
    if (channel >= 0 && channel < 16) {
        channelWidgets[channel]->setController(controller, value);
    }
}

void ChannelMonitor::onProgramChange(int channel, int program)
{
    if (channel >= 0 && channel < 16) {
        channelWidgets[channel]->setProgram(program);
    }
}

void ChannelMonitor::resetAllChannels()
{
    for (int i = 0; i < 16; ++i) {
        channelWidgets[i]->reset();
    }
}

void ChannelMonitor::setSoundMode(ChannelWidget::SoundMode mode)
{
    ChannelWidget::currentSoundMode = mode;

    // Update sound mode label with highly visible modern styles
    QString modeText;
    switch (mode) {
        case ChannelWidget::GM_MODE:
            modeText = "Mode: GM" + currentReliability.confusionHint + " (General MIDI)";
            soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #00e676; padding: 5px 10px; background-color: #112211; border: 1px solid #00e676; border-radius: 4px;");
            break;
        case ChannelWidget::MT32_MODE:
            modeText = "Mode: MT-32" + currentReliability.confusionHint + " (Roland MT-32)";
            soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #ff5252; padding: 5px 10px; background-color: #221111; border: 1px solid #ff5252; border-radius: 4px;");
            break;
        case ChannelWidget::GS_MODE:
            modeText = "Mode: GS" + currentReliability.confusionHint + " (Roland GS)";
            soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #2979ff; padding: 5px 10px; background-color: #111122; border: 1px solid #2979ff; border-radius: 4px;");
            break;
        case ChannelWidget::XG_MODE:
            modeText = "Mode: XG" + currentReliability.confusionHint + " (Yamaha XG)";
            soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #ffd600; padding: 5px 10px; background-color: #222211; border: 1px solid #ffd600; border-radius: 4px;");
            break;
        default:
            modeText = "Mode: Unknown" + currentReliability.confusionHint;
            soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #aaaaaa; padding: 5px 10px; background-color: #1c1c1c; border: 1px solid #aaaaaa; border-radius: 4px;");
            break;
    }
    soundModeLabel->setText(modeText);

    // Update all channel widgets to reflect new sound mode
    for (int i = 0; i < 16; ++i) {
        if (channelWidgets[i]->isChannelActive) {
            QString instrumentName = channelWidgets[i]->getInstrumentName(channelWidgets[i]->currentProgram);
            channelWidgets[i]->instrumentLabel->setText(instrumentName);
        }
    }
}

void ChannelMonitor::setImsMode(bool isIms, const QString& bankName, const QStringList& instruments, const QString& formatLabel)
{
    if (isIms) {
        int fixedHeight = m_channelAreaTopHeight + (16 * m_channelHeight); // 기존 크기 유지
        setFixedSize(m_fixedWidth, fixedHeight);

        m_imsInstruments = instruments;
        headerLabel->setText("OPL3 FM Voice Monitor");
        headerLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 15px; color: #00e5ff; padding: 6px;");
        
        soundModeLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 12px; color: #ff8a65; padding: 5px 10px; background-color: #241e1e; border: 1px solid #ff8a65; border-radius: 4px;");
        soundModeLabel->setText(QString("Mode: %1 (%2)").arg(formatLabel).arg(bankName.isEmpty() ? "Built-in Bank" : bankName));
        confidenceLabel->hide();
        midiChannelsContainer->hide();
        
        if (!imsLevelMeter) {
            imsLevelMeter = new ImsLevelMeterWidget(imsChannelsContainer);
            imsLevelMeter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            
            QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(imsChannelsContainer->layout());
            if (!layout) {
                layout = new QVBoxLayout(imsChannelsContainer);
                layout->setContentsMargins(0, 0, 0, 0);
            }
            layout->addWidget(imsLevelMeter);
        }
        
        imsLevelMeter->setInstruments(instruments);
        imsChannelsContainer->show();
        positionBesideMainWindow();
        
    } else {
        int fixedHeight = m_channelAreaTopHeight + (16 * m_channelHeight);
        setFixedSize(m_fixedWidth, fixedHeight);

        headerLabel->setText("MIDI Channel Monitor");
        headerLabel->setStyleSheet("font-family: 'Malgun Gothic', sans-serif; font-weight: bold; font-size: 15px; color: #00e5ff; padding: 6px;");
        
        setSoundMode(ChannelWidget::currentSoundMode);
        confidenceLabel->show();
        imsChannelsContainer->hide();
        midiChannelsContainer->show();
        positionBesideMainWindow();
    }
}

void ChannelMonitor::updateImsVolumes(const QList<int>& voiceVolumes, const QList<int>& instrumentVolumes)
{
    if (imsLevelMeter) {
        imsLevelMeter->setVolumes(voiceVolumes);
    }
    
    // Highlight instruments based on instrument-specific volume
    if (imsInstrumentListLabel && !m_imsInstruments.isEmpty()) {
        QString instText = "<b>[ IMS Instruments ]</b><br>";
        for (int i = 0; i < m_imsInstruments.size(); ++i) {
            bool isActive = (i < instrumentVolumes.size() && instrumentVolumes[i] > 20);

            // Same prefix convention as the voice-monitor name column:
            //   ""      → empty placeholder slot (dim grey, "--")
            //   "*name" → BNK miss, substituted (red, "(!) name")
            //   "name"  → resolved normally
            QString rawName = m_imsInstruments[i];
            bool substituted = rawName.startsWith('*');
            bool placeholder = rawName.isEmpty();
            QString shownName = substituted ? rawName.mid(1) : (placeholder ? "--" : rawName);

            QString color;
            if (substituted)      color = isActive ? "#FF5050" : "#823030";
            else if (placeholder) color = "#6E6E78";
            else                  color = isActive ? "#FFFF00" : "#CCCCCC";
            QString weight = isActive ? "bold" : "normal";

            QString prefix = substituted ? "(!) " : "";
            instText += QString("<span style='color: %1; font-weight: %2;'>%3. %4%5</span>&nbsp;&nbsp;&nbsp;")
                        .arg(color)
                        .arg(weight)
                        .arg(i+1, 2, 10, QChar('0'))
                        .arg(prefix)
                        .arg(shownName);

            if ((i + 1) % 4 == 0) instText += "<br>";
        }
        imsInstrumentListLabel->setText(instText);
    }
}

void ChannelMonitor::updateVoiceInstrumentNames(const QStringList& voiceNames)
{
    if (imsLevelMeter) {
        imsLevelMeter->setInstruments(voiceNames);
    }
}

void ChannelMonitor::refreshActiveChannels()
{
    // Force refresh all channels that have received program changes or are drum channels
    for (int i = 0; i < 16; ++i) {
        ChannelWidget* widget = channelWidgets[i];

        // Force activate channels that have program info or are drum channels
        if (widget->hasProgramChangeReceived || i == 9) {
            widget->setActive();
        }

        // Also update any currently active channels
        if (widget->isChannelActive) {
            widget->programLabel->setText(QString::number(widget->currentProgram + 1));
            widget->instrumentLabel->setText(widget->getInstrumentName(widget->currentProgram));
        }
    }
}

void ChannelMonitor::updateSoundModeReliability(const SoundModeReliability& reliability)
{
    // Store current reliability info
    currentReliability = reliability;

    // Build combined confidence text with reference note
    QString confidenceText = QString("Confidence: %1% (%2)")
                            .arg(reliability.confidenceScore)
                            .arg(reliability.detectionMethod);

    // Add reference note to the same line based on confidence score
    QString referenceNote;
    QString confidenceStyle;

    if (reliability.confidenceScore < 40) {
        referenceNote = " - Note: High chance of false positive";
        confidenceStyle = "font-size: 12px; color: #CCCCCC; padding: 3px;"; // Larger text, neutral gray
    } else if (reliability.confidenceScore < 80) {
        referenceNote = " - Note: Moderate confidence";
        confidenceStyle = "font-size: 12px; color: #DDDDAA; padding: 3px;"; // Larger text, light yellow-gray
    } else {
        referenceNote = ""; // No note for high confidence
        confidenceStyle = "font-size: 12px; color: #AAFFAA; padding: 3px;"; // Larger text, light green
    }

    // Combine confidence text with reference note
    QString fullText = confidenceText + referenceNote;

    confidenceLabel->setText(fullText);
    confidenceLabel->setStyleSheet(confidenceStyle);

    // Set tooltip with detailed evidence
    QString tooltipText = "Mode detection confidence: " + QString::number(reliability.confidenceScore) + "%\n";
    if (!reliability.evidenceList.isEmpty()) {
        tooltipText += "Evidence found:\n" + reliability.evidenceList.join("\n");
    } else {
        tooltipText += "No specific mode indicators found - using GM as default";
    }
    confidenceLabel->setToolTip(tooltipText);

    // No need to update mode label here - setSoundMode will handle it

    // Add evidence to sound mode label tooltip if available
    if (!reliability.evidenceList.isEmpty()) {
        soundModeLabel->setToolTip("Detection evidence:\n" + reliability.evidenceList.join("\n"));
    } else {
        soundModeLabel->setToolTip("No specific evidence - assumed GM compatible");
    }
}

void ChannelMonitor::closeEvent(QCloseEvent *event)
{
    emit windowClosed();
    event->accept();
}