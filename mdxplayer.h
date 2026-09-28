#ifndef MDXPLAYER_H
#define MDXPLAYER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QByteArray>
#include <atomic>
#include <mutex>
#include "mdxcore/mxdrv.h"

/**
 * @brief Sharp X68000 MDX / MDZ 음악 플레이어 클래스
 */
class MdxPlayer : public QObject
{
    Q_OBJECT
public:
    explicit MdxPlayer(QObject *parent = nullptr);
    ~MdxPlayer();

    bool loadFile(const QString& fileName);
    bool loadData(const QByteArray& mdxData, const QByteArray& pdxData = QByteArray(), const QString& fileName = QString());

    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing.load(std::memory_order_relaxed); }

    void setPosition(unsigned long positionMs);
    unsigned long getPosition() const;
    unsigned long getDuration() const { return m_duration; }

    void setVolume(int volume); // 0-127
    int getVolume() const { return m_volume.load(std::memory_order_relaxed); }

    // Channel-monitor feed, shaped like the OPL players' so the monitor can
    // stay in the one mode it already has for chip voices.
    QList<int>  getVoiceVolumes() const;
    QList<int>  getInstrumentVolumes() const;
    QStringList getVoiceInstrumentNames() const;
    QStringList getInstruments() const { return m_instruments; }
    QString     getBankName() const;

    int getCurrentBpm() const { return m_mxdrv.currentBpm(); }

    QString getTitle() const { return m_title; }
    QString getPdxName() const { return m_pdxName; }

    // Real-time key/tempo controls (F7~F11)
    void setUserTempoScale(int scale);
    int  getUserTempoScale() const;
    void setUserKeyTranspose(int semitones);
    // jmp's F12 virtual-stereo pattern, 1-9.
    void setStereoMode(int mode);
    int  getUserKeyTranspose() const;


    // Analogue-simulation DSP, the same chain and the same four levels the OPL
    // players use: a one-pole lowpass and a soft-clip.
    void setDspLevel(int level);
    int  getDspLevel() const { return m_dspLevel.load(std::memory_order_relaxed); }

    // Audio callback (called from JJoMeSynth)
    void renderAudio(float* output, unsigned int frameCount);

    // Fast title extractor without full playback loading
    static QString extractTitleQuick(const QString& fileName);

signals:
    void finished();
    void positionChanged(unsigned long positionMs);

private:
    Mxdrv m_mxdrv;
    mutable std::mutex m_mutex;

    std::atomic<bool> m_playing;
    std::atomic<bool> m_paused;
    std::atomic<int> m_volume;
    std::atomic<int> m_tempoScale;
    std::atomic<int> m_keyTranspose;
    std::atomic<int>  m_dspLevel{0};
    float m_lpfLast[2] = {0.0f, 0.0f};


    unsigned long m_duration;
    QString m_title;
    QString m_pdxName;
    QString m_currentFile;
    QStringList m_instruments;      // one entry per voice the file defines
};

#endif // MDXPLAYER_H
