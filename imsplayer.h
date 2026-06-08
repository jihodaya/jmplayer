#ifndef IMSPLAYER_H
#define IMSPLAYER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QByteArray>
#include <atomic>
#include <QElapsedTimer>
#include <mutex>
#include <map>
#include <adplug/adplug.h>
#include <adplug/nemuopl.h>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>

struct RollNote {
    uint64_t tick;
    int channel;
    int pitch;
};

class ImsFileProvider : public CProvider_Filesystem {
public:
    mutable QString loadedBankFile;
    QString mainFilePath;
    QString externalBankPath;

    void reset() { loadedBankFile.clear(); mainFilePath.clear(); externalBankPath.clear(); }
    void setMainFile(const QString& path) { mainFilePath = path; }
    void setExternalBank(const QString& path) { externalBankPath = path; }

    binistream *open(std::string filename) const override {
        QString qFilename = QString::fromLocal8Bit(filename.c_str());
        QString upperName = qFilename.toUpper();

        if (upperName.endsWith(".BNK") || upperName.endsWith(".SND") || upperName.endsWith(".TIM") || upperName.endsWith(".TBR")) {
            if (!externalBankPath.isEmpty()) {
                binistream *f = CProvider_Filesystem::open(QFile::encodeName(externalBankPath).constData());
                if (f) {
                    loadedBankFile = QFileInfo(externalBankPath).fileName();
                    return f;
                }
            }
            
            if (!mainFilePath.isEmpty()) {
                QFileInfo mainInfo(mainFilePath);
                QString baseName = mainInfo.completeBaseName();
                QString ext = QFileInfo(qFilename).suffix();
                
                QStringList sameNamePaths;
                sameNamePaths << mainInfo.absolutePath() + "/" + baseName + "." + ext;
                if (ext.toLower() != "bnk") {
                    sameNamePaths << mainInfo.absolutePath() + "/" + baseName + ".bnk";
                }
                
                for (const QString& path : sameNamePaths) {
                    binistream *f = CProvider_Filesystem::open(QFile::encodeName(path).constData());
                    if (f) {
                        loadedBankFile = QFileInfo(path).fileName();
                        return f;
                    }
                }
            }
        }

        binistream *f = CProvider_Filesystem::open(filename);
        if (f) {
            if (upperName.endsWith(".BNK") || upperName.endsWith(".SND") || upperName.endsWith(".TIM") || upperName.endsWith(".TBR")) {
                loadedBankFile = QFileInfo(qFilename).fileName();
            }
            return f;
        }

        if (upperName.endsWith(".BNK") || upperName.endsWith(".SND") || upperName.endsWith(".TIM") || upperName.endsWith(".TBR")) {
            QString baseName = QFileInfo(qFilename).fileName();
            QString appDirPath = QCoreApplication::applicationDirPath();
            
            QStringList searchPaths;
            searchPaths << QDir(appDirPath).filePath(baseName);
            searchPaths << QDir(appDirPath).filePath("ims/" + baseName);
            searchPaths << QDir::current().filePath("ims/" + baseName);
            
            for (const QString& path : searchPaths) {
                binistream *sf = CProvider_Filesystem::open(QFile::encodeName(path).constData());
                if (sf) {
                    loadedBankFile = QFileInfo(path).fileName();
                    return sf;
                }
            }
        }
        return nullptr;
    }
};

class ImsPlayer : public QObject
{
    Q_OBJECT
public:
    explicit ImsPlayer(QObject *parent = nullptr);
    ~ImsPlayer();

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

    QList<int> getVoiceVolumes() const;
    QList<int> getInstrumentVolumes() const;
    QStringList getVoiceInstrumentNames() const;

    QString getTitle() const { return m_title; }
    QString getBankName() const { return m_bankName; }
    QStringList getInstruments() const { return m_instruments; }
    
    void setExternalBankPath(const QString& path) { m_externalBankPath = path; }
    QString getExternalBankPath() const { return m_externalBankPath; }

    static QString extractTitleQuick(const QString& fileName);
    static QString extractTitleQuick(const QByteArray& fileData, const QString& ext);

    int getBasicTempo() const { return m_basicTempo; }
    int getTickBeat() const   { return m_nTickBeat; }
    uint64_t getCurrentTick() const { return m_currentTick.load(); }

    void setUserTempoScale(int scale);
    int getUserTempoScale() const;
    void setUserKeyTranspose(int key);
    int getUserKeyTranspose() const;
    int getCurrentBpm() const;

    // Audio rendering for miniaudio callback
    void renderAudio(float* output, unsigned int frameCount);

    void forceUpdateOplStereo();

    const QList<RollNote>& getRollNotes() const { return m_rollNotes; }

signals:
    void finished();
    void positionChanged(unsigned long positionMs);

private:
    void performSeek(unsigned long positionMs);

    CPlayer* m_player;
    CNemuopl* m_opl;
    ImsFileProvider m_provider;
    mutable std::mutex m_playerMutex;
    std::atomic<bool> m_playing;
    unsigned long m_duration;
    unsigned int m_sampleRate;
    std::atomic<unsigned long> m_position;
    std::atomic<float> m_sampleCounter;
    std::atomic<int> m_volume;
    
    QString m_title;
    QStringList m_instruments;
    bool m_isSop = false;   // true for .sop: read exact per-voice instrument from CsopPlayer
    QString m_bankName;
    QString m_externalBankPath;
    QList<RollNote> m_rollNotes;

    int m_basicTempo;
    int m_nTickBeat;
    std::atomic<uint64_t> m_currentTick;
    std::atomic<int> m_userTempoScale;
    std::atomic<int> m_userKeyTranspose;

    std::atomic<int> m_voiceVolumes[18];
    std::atomic<int> m_instVolumes[256];
    
    std::atomic<int> m_dspLevel;
    float m_lpfLastL, m_lpfLastR;
    float m_lastOutL = 0.0f, m_lastOutR = 0.0f; // last emitted sample (seek fade-out)

    std::atomic<bool> m_needsFadeIn;
    std::atomic<int> m_fadeCounter;
    float m_customRefresh;
    double m_positionRemainder;
    QElapsedTimer m_seekTimer;



    bool m_isRol = false;
    bool m_isIms = false;
    bool m_isVgm = false;
    std::map<uint64_t, int> m_vgmPatchMap;
    int m_vgmNextPatchNum = 1;
    char m_cachedVoiceInsts[20][32];
    char m_cachedVoiceNotes[20][8];
    uint8_t m_cachedVoiceVols[20];
    uint8_t m_cachedVoiceKeyOn[20];
};

#endif // IMSPLAYER_H
