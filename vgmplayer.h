#ifndef VGMPLAYER_H
#define VGMPLAYER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QStringList>
#include <atomic>
#include <mutex>

#include "vgmcore/vgmbackend.h"

/**
 * @brief Video Game Music (.vgm / .vgz) 플레이어
 *
 * The Qt side of `vgmcore/`, shaped like MdxPlayer so MainWindow treats it the
 * same way. Note what it is *not* for: a VGM whose only chip is an OPL still
 * goes to AdPlug through ImsPlayer, which has played them correctly for a long
 * time. `handlesFile()` is what decides, and it decides by reading the header
 * rather than by extension.
 */
class VgmPlayer : public QObject
{
    Q_OBJECT
public:
    explicit VgmPlayer(QObject *parent = nullptr);
    ~VgmPlayer();

    // True when this file names a chip we play and AdPlug would not. A file
    // that only wants OPL is left where it was.
    static bool handlesFile(const QString& fileName);
    static QString extractTitleQuick(const QString& fileName);
    // What chips the file asks for, marking the ones this build cannot play.
    // Empty if the file is not a VGM at all.
    static QString describeChips(const QString& fileName);

    bool loadFile(const QString& fileName);

    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing.load(std::memory_order_relaxed); }

    void setPosition(unsigned long positionMs);
    unsigned long getPosition() const;
    unsigned long getDuration() const { return m_duration; }

    void setVolume(int volume);          // 0-127
    int  getVolume() const { return m_volume.load(std::memory_order_relaxed); }

    QString getTitle() const { return m_title; }
    QString getChips() const;

    // Channel-monitor feed, shaped like the OPL and MDX players' so the monitor
    // can stay in the one mode it has for chip voices.
    QList<int>  getVoiceVolumes() const;
    QList<int>  getInstrumentVolumes() const;
    QStringList getVoiceInstrumentNames() const;
    QStringList getInstruments() const { return m_instruments; }
    QString     getBankName() const;


    // Analogue-simulation DSP, the same chain and the same four levels the OPL
    // players use: a one-pole lowpass and a soft-clip.
    void setDspLevel(int level);
    // jmp's F12 virtual-stereo pattern, 1-9.
    void setStereoMode(int mode);
    int  getDspLevel() const { return m_dspLevel.load(std::memory_order_relaxed); }

    void renderAudio(float* output, unsigned int frameCount);

signals:
    void finished();

private:
    VgmBackend m_vgm;
    mutable std::mutex m_mutex;

    std::atomic<bool> m_playing;
    std::atomic<bool> m_paused;
    std::atomic<int>  m_volume;
    std::atomic<int>  m_dspLevel{0};
    float m_lpfLast[2] = {0.0f, 0.0f};


    unsigned long m_duration = 0;
    QString m_title;
    QString m_currentFile;
    QStringList m_instruments;      // one row per chip voice
};

#endif // VGMPLAYER_H
