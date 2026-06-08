#ifndef PIANOROLLWINDOW_H
#define PIANOROLLWINDOW_H

#include <QDialog>
#include <QTimer>
#include <QPainter>
#include "imsplayer.h"
#include "gybplayer.h"

class PianoRollWindow : public QDialog
{
    Q_OBJECT

public:
    explicit PianoRollWindow(QWidget *parent = nullptr);
    ~PianoRollWindow();

public slots:
    void onNoteOn(int channel, int pitch, int velocity);
    void onNoteOff(int channel, int pitch);
    void clearNotes();
    void setImsPlayer(ImsPlayer* player) { m_imsPlayer = player; }
    void setGybPlayer(GybPlayer* player) { m_gybPlayer = player; }
    void setOkaPlayer(class OkaPlayer* player) { m_okaPlayer = player; }
    void setMidiPlayer(class MidiPlayer* player) { m_midiPlayer = player; }

signals:
    void windowClosed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onUpdateTimer();

private:
    ImsPlayer* m_imsPlayer;
    GybPlayer* m_gybPlayer = nullptr;
    class OkaPlayer* m_okaPlayer = nullptr;
    class MidiPlayer* m_midiPlayer = nullptr;
    QTimer* m_updateTimer;
    QMap<int, int> m_activeNotes; // pitch -> channel (for coloring)


    static const int KEY_HEIGHT = 10;
    static const int TICK_WIDTH = 2; // pixels per tick
    static const int WINDOW_TICKS = 800; // Number of ticks visible on screen
};

#endif // PIANOROLLWINDOW_H
