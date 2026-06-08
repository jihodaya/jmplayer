#include "pianorollwindow.h"
#include "okaplayer.h"
#include "midiplayer.h"
#include <QApplication>
#include <QMainWindow>
#include <QPainter>
#include <QColor>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

PianoRollWindow::PianoRollWindow(QWidget *parent)
    : QDialog(parent, Qt::Tool | Qt::WindowTitleHint | Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint)
    , m_imsPlayer(nullptr)
{
    setWindowTitle("Keyboard View");
    resize(800, 140); // Initial size
    setStyleSheet("background-color: #2b2b2b;"); // Match main window dark theme

#ifdef _WIN32
    if (winId()) {
        HWND hwnd = (HWND)winId();
        BOOL darkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)); 
        BOOL useImmersiveDarkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 19, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode)); 
    }
#endif

    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, &QTimer::timeout, this, &PianoRollWindow::onUpdateTimer);
    m_updateTimer->start(33); // ~30 fps
}

PianoRollWindow::~PianoRollWindow()
{
}



void PianoRollWindow::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
    emit windowClosed();
}

void PianoRollWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
#ifdef _WIN32
    if (winId()) {
        HWND hwnd = (HWND)winId();
        BOOL darkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode)); 
        BOOL useImmersiveDarkMode = TRUE;
        ::DwmSetWindowAttribute(hwnd, 19, &useImmersiveDarkMode, sizeof(useImmersiveDarkMode)); 
        ::SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
#endif
}

void PianoRollWindow::onNoteOn(int channel, int pitch, int velocity)
{
    if (velocity > 0) {
        m_activeNotes[pitch] = channel;
    } else {
        m_activeNotes.remove(pitch);
    }
    update();
}

void PianoRollWindow::onNoteOff(int channel, int pitch)
{
    if (m_activeNotes.contains(pitch) && m_activeNotes[pitch] == channel) {
        m_activeNotes.remove(pitch);
        update();
    }
}

void PianoRollWindow::clearNotes()
{
    m_activeNotes.clear();
    update();
}

void PianoRollWindow::onUpdateTimer()
{
    // Always update if visible to handle real-time signal changes and OPL polling
    if (isVisible()) {
        update();
    }
}

void PianoRollWindow::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Colors for channels
    QColor chanColors[18] = {
        QColor("#FF5252"), QColor("#FF4081"), QColor("#E040FB"), QColor("#7C4DFF"),
        QColor("#536DFE"), QColor("#448AFF"), QColor("#40C4FF"), QColor("#18FFFF"),
        QColor("#64FFDA"), QColor("#69F0AE"), QColor("#B2FF59"), QColor("#EEFF41"),
        QColor("#FFFF00"), QColor("#FFD740"), QColor("#FFAB40"), QColor("#FF6E40"),
        QColor("#BCAAA4"), QColor("#EEEEEE")
    };

    QMap<int, QColor> activePitches;

    // 1. Check IMS/OPL pre-scanned notes if active
    if (m_imsPlayer && m_imsPlayer->isPlaying()) {
        uint64_t currentTick = m_imsPlayer->getCurrentTick();
        const QList<RollNote>& notes = m_imsPlayer->getRollNotes();
        int transpose = m_imsPlayer->getUserKeyTranspose();

        // Find currently active notes (notes played within the last ~100ms)
        // 15 ticks is a good approximation for a brief highlight
        uint64_t startTick = (currentTick > 15) ? currentTick - 15 : 0;
        auto it = std::lower_bound(notes.begin(), notes.end(), startTick, 
            [](const RollNote& note, uint64_t t) { return note.tick < t; });

        for (; it != notes.end(); ++it) {
            if (it->tick > currentTick + 2) break;
            int pitch = it->pitch;
            // OPL Drum channels (6..10) bypass transpose
            if (!(it->channel >= 6 && it->channel <= 10)) {
                pitch += transpose;
            }
            if (pitch >= 0 && pitch <= 127) {
                activePitches[pitch] = chanColors[it->channel % 18];
            }
        }
    } else if (m_gybPlayer && m_gybPlayer->isPlaying()) {
        uint64_t currentTick = m_gybPlayer->getCurrentTick();
        const QList<GybRollNote>& notes = m_gybPlayer->getRollNotes();
        int transpose = m_gybPlayer->getUserKeyTranspose();

        uint64_t startTick = (currentTick > 15) ? currentTick - 15 : 0;
        auto it = std::lower_bound(notes.begin(), notes.end(), startTick, 
            [](const GybRollNote& note, uint64_t t) { return note.tick < t; });

        for (; it != notes.end(); ++it) {
            if (it->tick > currentTick + 2) break;
            int pitch = it->pitch;
            // OPL Drum channels (6..10) bypass transpose
            if (!(it->channel >= 6 && it->channel <= 10)) {
                pitch += transpose;
            }
            if (pitch >= 0 && pitch <= 127) {
                activePitches[pitch] = chanColors[it->channel % 18];
            }
        }
    } else if (m_okaPlayer && m_okaPlayer->isPlaying()) {
        uint64_t currentTick = m_okaPlayer->getCurrentTick();
        const QList<OkaRollNote>& notes = m_okaPlayer->getRollNotes();
        int transpose = m_okaPlayer->getUserKeyTranspose();

        uint64_t startTick = (currentTick > 15) ? currentTick - 15 : 0;
        auto it = std::lower_bound(notes.begin(), notes.end(), startTick, 
            [](const OkaRollNote& note, uint64_t t) { return note.tick < t; });

        for (; it != notes.end(); ++it) {
            if (it->tick > currentTick + 2) break;
            int pitch = it->pitch + transpose;
            if (pitch >= 0 && pitch <= 127) {
                activePitches[pitch] = chanColors[it->channel % 18];
            }
        }
    }

    // 2. Overlay real-time MIDI signals (for MID/NOB or real-time OPL feedback)
    int midiTranspose = 0;
    if (m_midiPlayer) {
        midiTranspose = m_midiPlayer->getUserKeyTranspose();
    }

    for (auto it = m_activeNotes.begin(); it != m_activeNotes.end(); ++it) {
        int ch = it.value();
        int pitch = it.key();
        if (ch != 9) { // Skip drum channel (usually channel 10, index 9)
            pitch += midiTranspose;
        }
        if (pitch >= 0 && pitch <= 127) {
            activePitches[pitch] = chanColors[ch % 18];
        }
    }

    // Keyboard configuration
    int startNote = 24; // C1
    int numOctaves = 7; // C1 to B7 (84 keys)
    int numWhiteKeys = numOctaves * 7;
    
    float whiteKeyWidth = (float)width() / numWhiteKeys;
    float whiteKeyHeight = height();
    float blackKeyWidth = whiteKeyWidth * 0.65f;
    float blackKeyHeight = whiteKeyHeight * 0.6f;
    
    float keyboardX = 0;
    float keyboardY = 0;

    // Gradients for unpressed keys
    QLinearGradient whiteGrad(0, 0, 0, whiteKeyHeight);
    whiteGrad.setColorAt(0, QColor("#F0F0F0"));
    whiteGrad.setColorAt(1, QColor("#FFFFFF"));

    QLinearGradient blackGrad(0, 0, 0, blackKeyHeight);
    blackGrad.setColorAt(0, QColor("#333333"));
    blackGrad.setColorAt(0.2, QColor("#111111"));
    blackGrad.setColorAt(1, QColor("#000000"));

    // 1. Draw white keys
    int whiteIdx = 0;
    for (int i = 0; i < numOctaves * 12; ++i) {
        int note = startNote + i;
        int noteInOctave = note % 12;
        bool isBlack = (noteInOctave==1 || noteInOctave==3 || noteInOctave==6 || noteInOctave==8 || noteInOctave==10);
        
        if (!isBlack) {
            float x = keyboardX + whiteIdx * whiteKeyWidth;
            QRectF keyRect(x, keyboardY, whiteKeyWidth, whiteKeyHeight);
            
            if (activePitches.contains(note)) {
                QColor c = activePitches[note];
                QLinearGradient activeWhite(0, 0, 0, whiteKeyHeight);
                activeWhite.setColorAt(0, c.lighter(130));
                activeWhite.setColorAt(0.7, c);
                activeWhite.setColorAt(1, c.darker(110));
                painter.fillRect(keyRect, activeWhite);
            } else {
                painter.fillRect(keyRect, whiteGrad);
            }
            // Draw inner shadow/border effect
            painter.setPen(QColor("#777777"));
            painter.drawLine(QPointF(x, keyboardY), QPointF(x, keyboardY + whiteKeyHeight));
            whiteIdx++;
        }
    }

    // 2. Draw black keys
    whiteIdx = 0;
    for (int i = 0; i < numOctaves * 12; ++i) {
        int note = startNote + i;
        int noteInOctave = note % 12;
        bool isBlack = (noteInOctave==1 || noteInOctave==3 || noteInOctave==6 || noteInOctave==8 || noteInOctave==10);
        
        if (!isBlack) {
            whiteIdx++;
        } else {
            float x = keyboardX + whiteIdx * whiteKeyWidth - (blackKeyWidth / 2.0f);
            QRectF keyRect(x, keyboardY, blackKeyWidth, blackKeyHeight);
            
            if (activePitches.contains(note)) {
                QColor c = activePitches[note];
                QLinearGradient activeBlack(0, 0, 0, blackKeyHeight);
                activeBlack.setColorAt(0, c.darker(150));
                activeBlack.setColorAt(0.8, c);
                activeBlack.setColorAt(1, c.darker(120));
                painter.fillRect(keyRect, activeBlack);
            } else {
                painter.fillRect(keyRect, blackGrad);
            }
            
            // Draw subtle 3D highlight
            painter.setPen(QColor(255, 255, 255, 30));
            painter.drawLine(QPointF(x + 1, keyboardY + 1), QPointF(x + blackKeyWidth - 2, keyboardY + 1));
            painter.drawLine(QPointF(x + 1, keyboardY + 1), QPointF(x + 1, keyboardY + blackKeyHeight - 2));
            
            painter.setPen(QColor("#000000"));
            painter.drawRect(keyRect);
        }
    }
}
