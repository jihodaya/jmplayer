#ifndef GYBPLAYER_H
#define GYBPLAYER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <atomic>
#include <mutex>
#include <adplug/nemuopl.h>
#include <adplug/fprovide.h>

class GybBackend;

struct GybRollNote {
    uint64_t tick;
    int channel;
    int pitch;
};

class GybPlayer : public QObject
{
    Q_OBJECT
public:
    explicit GybPlayer(QObject *parent = nullptr);
    ~GybPlayer();

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

    int getBasicTempo() const { return m_basicTempo; }
    int getTickBeat() const { return m_tickBeat; }
    uint64_t getCurrentTick() const { return m_currentTick.load(); }

    void renderAudio(float* output, unsigned int frameCount);
    void forceUpdateOplStereo();
    const QList<GybRollNote>& getRollNotes() const { return m_rollNotes; }

    static QString extractTitleQuick(const QString& fileName);

signals:
    void finished();
    void positionChanged(unsigned long positionMs);

private:
    // OPL chip + AdPlug backend doing the actual playback. The backend uses
    // CcomposerBackend's BNK lookup + register writes (identical to IMS), so
    // GYB and IMS now share the OPL programming path.
    class InterceptingOpl;
    InterceptingOpl* m_opl;
    GybBackend*      m_backend;
    CProvider_Filesystem m_provider;

    mutable std::mutex m_playerMutex;
    std::atomic<bool> m_playing;
    unsigned int      m_sampleRate;
    std::atomic<unsigned long> m_position;
    std::atomic<float>         m_sampleCounter;
    std::atomic<int>           m_volume;
    std::atomic<int>           m_dspLevel;
    std::atomic<uint64_t>      m_currentTick;

    QString m_title;
    QString m_bankName;
    QString m_externalBankPath;
    QStringList m_instrumentNames;  // slot list with slot 0 stripped (IMS-style row layout)
    int m_basicTempo;
    int m_tickBeat;
    unsigned long m_duration;

    QList<GybRollNote> m_rollNotes; // currently unused; kept for piano-roll API compat

    // DSP (mirrors ImsPlayer)
    float m_lpfLastL, m_lpfLastR;
    float m_lastOutL = 0.0f, m_lastOutR = 0.0f; // last emitted sample (seek fade-out)
    std::atomic<bool> m_needsFadeIn;
    std::atomic<int>  m_fadeCounter;

    // Voice/instrument level meters (captured by InterceptingOpl).
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

};

#endif // GYBPLAYER_H
