#ifndef CHANNELMONITOR_H
#define CHANNELMONITOR_H

#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QFrame>
#include <QTimer>
#include <QIcon>
#include <QListWidget>
#include "midiplayer.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

class ImsLevelMeterWidget; // For DOS-style IMS level meter
class MidiLevelMeterWidget; // OPL-themed big level meter for the 16 MIDI channels

class ChannelWidget : public QFrame
{
    Q_OBJECT

public:
    explicit ChannelWidget(int channelNumber, QWidget *parent = nullptr);

public slots:
    void setNote(int note, int velocity);
    void clearNote(int note);
    void setController(int controller, int value);
    void setProgram(int program);
    void setLevel(int volume);  // IMS용: 노트 없이 볼륨바만 업데이트
    void reset();
    void setInactive();
    void setActive();
    void setImsChannelMode(const QString& instrumentName); // IMS 전용 레이아웃
    QString getInstrumentName(int program);

public:
    enum SoundMode {
        // The file said nothing about which module it targets. Distinct from
        // GM_MODE, which now means the file actually declared GM - guessing GM
        // from silence was reporting a certainty the file does not contain.
        // Instrument names fall back to the GM table either way.
        UNKNOWN_MODE = -1,
        GM_MODE = 0,    // General MIDI
        MT32_MODE = 1,  // Roland MT-32
        GS_MODE = 2,    // Roland GS
        XG_MODE = 3     // Yamaha XG
    };

private:
    void setupUI();
    QString getGMInstrumentName(int program);
    QString getMT32InstrumentName(int program);

    int channel;

public:
    static SoundMode currentSoundMode;
    QLabel *channelLabel;
    QLabel *programLabel;
    QLabel *instrumentLabel;
    QProgressBar *volumeBar;
    QLabel *notesLabel;
    QTimer *decayTimer;

public:
    int currentVolume;
    int currentProgram;
    bool hasProgramChangeReceived;
    QStringList activeNotes;
    bool isChannelActive;
    QTimer *inactivityTimer;
};

class ChannelMonitor : public QMainWindow
{
    Q_OBJECT

public:
    explicit ChannelMonitor(QWidget *parent = nullptr);
    ~ChannelMonitor();

public slots:
    void onNoteOn(int channel, int note, int velocity);
    void onNoteOff(int channel, int note);
    void onControllerChange(int channel, int controller, int value);
    void onProgramChange(int channel, int program);
    void resetAllChannels();
    void setSoundMode(ChannelWidget::SoundMode mode);
    void setImsMode(bool isIms, const QString& bankName = QString(), const QStringList& instruments = QStringList(), const QString& formatLabel = "IMS");
    void updateImsVolumes(const QList<int>& voiceVolumes, const QList<int>& instrumentVolumes);
    // Dynamically refresh per-voice instrument names (GYB: voice→program mapping changes at runtime).
    void updateVoiceInstrumentNames(const QStringList& voiceNames);
    void positionBesideMainWindow();
    void refreshActiveChannels();
    void updateSoundModeReliability(const SoundModeReliability& reliability);

protected:
    void closeEvent(QCloseEvent *event) override;

signals:
    void windowClosed();

private:
    void setupUI();

    QWidget *centralWidget;
    QVBoxLayout *mainLayout;
    QLabel *headerLabel;
    QLabel *soundModeLabel;
    QLabel *confidenceLabel;
    
    QWidget *midiChannelsContainer;
    ChannelWidget *channelWidgets[16];

    // OPL-themed MIDI display: the 16 ChannelWidgets above stay alive as the
    // data/state holders (program, naming, activity), but what the user sees is
    // this big OPL-style meter fed in parallel from the same slots.
    QWidget *midiMeterContainer = nullptr;
    MidiLevelMeterWidget *midiLevelMeter = nullptr;

    QWidget *imsChannelsContainer;
    QList<ChannelWidget*> imsChannelWidgets;
    ImsLevelMeterWidget *imsLevelMeter = nullptr;
    QLabel *imsInstrumentListLabel = nullptr;
    QStringList m_imsInstruments;

    int m_fixedWidth;
    int m_channelAreaTopHeight;
    int m_channelHeight;

    SoundModeReliability currentReliability;

    QWidget *parentMainWindow;
};

#endif // CHANNELMONITOR_H