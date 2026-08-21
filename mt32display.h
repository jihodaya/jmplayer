#ifndef MT32DISPLAY_H
#define MT32DISPLAY_H

// The MT-32's front-panel display, drawn rather than borrowed.
//
// munt's own Qt GUI has a window that looks like this, but that program is
// GPL-3.0 while only its mt32emu *library* is LGPL - lifting the widget would
// take jmp's licence with it. Drawing it here costs a couple of hundred lines
// and leaves the frame ours to style, which is what was wanted anyway.
//
// Everything shown comes from the library: getDisplayState() hands back the
// twenty characters the hardware would be showing and whether the MIDI MESSAGE
// lamp is lit. There is no clever state tracking here - the display is polled,
// because the emulator updates it during rendering and a timer is both simpler
// and cheaper than marshalling a callback off the audio thread.

#include <QWidget>

class Mt32Synth;
class QComboBox;

class QTimer;
class QEvent;

// The panel itself. Split out so the surrounding window is ordinary Qt
// widgets and only this part is custom painting.
class Mt32Lcd : public QWidget
{
    Q_OBJECT

public:
    explicit Mt32Lcd(QWidget* parent = nullptr);

    void setText(const QString& text);
    void setLed(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    bool    m_bLed = false;
};

class Mt32Display : public QWidget
{
    Q_OBJECT

public:
    explicit Mt32Display(Mt32Synth* pSynth, QWidget* parent = nullptr);

    // Re-reads the ROM folder and rebuilds the selector. Safe to call at any
    // time; keeps the current selection if it is still present.
    void refreshRomList();

    // Parks the window above jmp, the same rule the SC-55 panel follows.
    void placeBesideMainWindow();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

signals:
    // Changing the ROM rebuilds the whole machine, so a song playing through it
    // has to be taken out of the way first: the sequencer would otherwise keep
    // feeding a synth that is being torn down, which is heard as the sound
    // breaking up (reported 2026-08-21). The host stops on the first and starts
    // the song again on the second. Both are no-ops when nothing is connected,
    // which is how tests/mt32ui.cpp drives this panel on its own.
    void romChangeAboutToApply();
    void romChangeApplied();

private slots:
    void onRomChanged(int index);
    void onPoll();

private:
    void updateTitle();

    Mt32Synth* m_pSynth = nullptr;   // not owned
    Mt32Lcd*   m_pLcd   = nullptr;
    QComboBox* m_pRoms  = nullptr;
    QTimer*    m_pTimer = nullptr;
    bool       m_bPlaced = false;
};

#endif // MT32DISPLAY_H
