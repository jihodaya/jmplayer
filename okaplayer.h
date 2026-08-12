#ifndef OKAPLAYER_H
#define OKAPLAYER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <atomic>
#include <mutex>
#include <adplug/nemuopl.h>

class OkaBackend;

struct OkaRollNote {
    uint64_t tick;
    int channel;
    int pitch;
};

class OkaPlayer : public QObject
{
    Q_OBJECT
public:
    explicit OkaPlayer(QObject *parent = nullptr);
    ~OkaPlayer();

    bool loadFile(const QString& fileName);
    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing.load(); }

    void setPosition(unsigned long positionMs);
    unsigned long getPosition() const;
    unsigned long getDuration() const { return m_duration; }

    void setVolume(int volume);
    int getVolume() const { return m_volume.load(); }

    void setDspLevel(int level);
    int getDspLevel() const { return m_dspLevel.load(); }

    QString getTitle() const { return m_title; }

    // IMS-compatible interface
    QString getBankName() const { return m_bankName; }
    QStringList getInstruments() const { return m_instrumentNames; }
    void setExternalBankPath(const QString& path) { m_externalBankPath = path; }
    QString getExternalBankPath() const { return m_externalBankPath; }
    QList<int> getVoiceVolumes() const;
    QList<int> getInstrumentVolumes() const;
    QStringList getVoiceInstrumentNames() const;

    // User tempo/key controls
    void setUserTempoScale(int scale);
    int  getUserTempoScale() const;
    void setUserKeyTranspose(int semitones);
    int  getUserKeyTranspose() const;
    int  getCurrentBpm() const;

    uint64_t getCurrentTick() const { return m_currentTick.load(); }

    void renderAudio(float* output, unsigned int frameCount);
    void forceUpdateOplStereo();
    const QList<OkaRollNote>& getRollNotes() const { return m_rollNotes; }
    const QList<unsigned long>& getLyricMarkerTicks() const { return m_lyricMarkerTicks; }

signals:
    void finished();
    void positionChanged(unsigned long positionMs);

private:
    class InterceptingOpl;
    InterceptingOpl* m_opl;
    OkaBackend*      m_backend;

    mutable std::mutex m_playerMutex;
    std::atomic<bool> m_playing;
    unsigned int      m_sampleRate;
    std::atomic<unsigned long> m_position;
    double m_positionRemainder = 0.0;  // sub-millisecond carry (audio thread)
    std::atomic<float>         m_sampleCounter;
    std::atomic<int>           m_volume;
    std::atomic<int>           m_dspLevel;
    std::atomic<uint64_t>      m_currentTick;
    // OPL-tunnel song clock (audio thread only) - see ImsPlayer's member of
    // the same name: monotonic sample count stamping each tick's writes.
    double m_tunnelClockSamples = 0.0;

    QString m_title;
    QString m_bankName;
    QString m_externalBankPath;
    QStringList m_instrumentNames;
    unsigned long m_duration;

    QList<OkaRollNote> m_rollNotes;
    QList<unsigned long> m_lyricMarkerTicks;

    // DSP (mirrors ImsPlayer)
    float m_lpfLastL, m_lpfLastR;
    float m_lastOutL = 0.0f, m_lastOutR = 0.0f; // last emitted sample (seek fade-out)
    std::atomic<bool> m_needsFadeIn;
    std::atomic<int>  m_fadeCounter;

    // Voice/instrument level meters
    mutable std::atomic<int> m_voiceLevelOut[18];
    mutable std::atomic<int> m_instLevelOut[64];

    bool m_oplKeyOn[18];
    int  m_oplVolume[18];
    int  m_oplRegA[18];
    int  m_oplRegB[18];
    char m_cachedVoiceInsts[20][32];
    char m_cachedVoiceNotes[20][8];
    uint8_t m_cachedVoiceVols[20];
    uint8_t m_cachedVoiceKeyOn[20];

    // See ImsPlayer's identical member: the channel-monitor snapshot below is
    // display-only but allocates heavily (QString/QByteArray per voice), and
    // running it on every ticked audio callback put thousands of heap
    // allocations a second in the realtime thread, where the heap lock is
    // shared with the GUI (2026-07-27).
    unsigned m_uiSnapshotFrames = 0;

};

#endif // OKAPLAYER_H
