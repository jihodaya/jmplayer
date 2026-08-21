#ifndef MIDIPLAYER_H
#define MIDIPLAYER_H

#include <QObject>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <fstream>
#include <sstream>
#include "jjomesynth.h"
#include "midireset.h"
#include "sc55bridge.h"
#include "mt32synth.h"

#pragma comment(lib, "winmm.lib")

struct MidiEvent {
    unsigned long deltaTime;
    unsigned long long absoluteTimeUs; // Absolute time in microseconds
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    bool isMetaEvent;
    bool isSysExEvent;
    std::vector<unsigned char> metaData;
    std::vector<unsigned char> sysExData;
};

struct MidiTrack {
    std::vector<MidiEvent> events;
    unsigned long currentEventIndex;
    unsigned long currentTime;
};

struct SoundModeReliability {
    int detectedMode;
    int confidenceScore;      // 0-100, higher = more confident
    QString detectionMethod;  // How the mode was detected
    QStringList evidenceList; // List of evidence found
    bool hasStrongEvidence;   // True if score >= 80
    int alternativeMode;      // Most likely alternative mode (-1 if none)
    QString confusionHint;    // Text hint for possible confusion (e.g., "(or MT-32)")
    QMap<int, int> allModeScores; // All mode scores for ranking
};

enum DetectionStrength {
    WEAK_DETECTION = 0,     // Score < 40
    MODERATE_DETECTION = 1, // Score 40-79
    STRONG_DETECTION = 2    // Score >= 80
};

class MidiPlayer; // Forward declaration

// Background thread for MIDI playback to ensure UI doesn't block timing
class PlaybackThread : public QThread {
public:
    explicit PlaybackThread(MidiPlayer* player) : m_player(player), m_running(false) {}
    ~PlaybackThread() override {}
    
    void stop() {
        m_running = false;
    }
protected:
    void run() override;
private:
    MidiPlayer* m_player;
    std::atomic<bool> m_running; // atomic for correct cross-thread visibility/ordering
};

class MidiPlayer : public QObject
{
    Q_OBJECT
    friend class PlaybackThread;

public:
    explicit MidiPlayer(QObject *parent = nullptr);
    ~MidiPlayer();

    QStringList getAvailableDevices();
    bool connectToDevice(int deviceId);
    bool connectToDeviceByName(const QString& deviceName);
    void disconnect(bool async = true);
    bool isConnected() const;

    // Public raw-SysEx send for the OPL register tunnel (opltunnelsender.cpp).
    // Called from the tunnel's sender thread; forwards to the private
    // sendSysExMessage (midiOutLongMsg to the connected device).
    void sendRawSysEx(const std::vector<unsigned char> &data) { sendSysExMessage(data); }

    // Change one channel's instrument on the device right now, without touching
    // the loaded stream. The patch dialog uses this so an instrument picked
    // mid-song is heard immediately; the stream itself is rebuilt on OK.
    void sendLiveProgramChange(int channel, int bankMsb, int program);

    // Sound-module reset before each new song (see midireset/midireset.h).
    // MainWindow reads/writes these to wire the settings UI; the actual send
    // happens inside play()'s new-song branch.
    MidiReset& midiReset() { return m_midiReset; }

    // Internal Synth setting
    void setUseInternalSynth(bool useInternal, const QString& soundFontPath = QString());
    bool isUsingInternalSynth() const { return m_useInternalSynth; }

    // Nuked-SC55 over a named pipe (sc55/sc55bridge.h) - a third destination
    // alongside the internal synth and a real WinMM device. Selected from the
    // device list like any other; MainWindow calls this instead of
    // connectToDevice() when the user picks it.
    bool connectToSc55();
    Sc55Bridge* sc55Bridge() const { return m_pSc55; }
    bool isUsingSc55() const { return m_pSc55 && m_bUseSc55; }

    // MT-32 / CM-32L through munt (mt32synth.h) - a fourth destination. Unlike
    // the SC-55 this one is a library in this process, so there is nothing to
    // launch and nothing to connect to: it renders into the same audio device
    // the internal synth uses, via JJoMeSynth.
    bool connectToMt32();
    Mt32Synth* mt32Synth() const { return m_pMt32; }
    bool isUsingMt32() const { return m_pMt32 && m_bUseMt32; }

    bool loadMidiFile(const QString &filename);
    void setIsNobFile(bool isNob); // NOB 파일 여부 설정
    void play();
    void pause();
    void stop();
    bool isPlaying() const;

    void setVolume(int volume); // 0-127
    int getVolume() const;

    QString getCurrentFile() const;
    unsigned long getCurrentPosition() const; // in milliseconds
    unsigned long getTotalDuration() const; // in milliseconds
    unsigned long getTotalTicks() const; // Total MIDI ticks
    void setPosition(unsigned long position); // in milliseconds

    QString getTrackInfo() const;
    QStringList extractLyrics() const;

    // Tick of each sung syllable, taken straight from the lyric events. Pairs
    // one-for-one with the syllables in extractLyrics().
    QList<unsigned long> extractLyricSyllableTicks() const;

    // Decode a MIDI text/lyric meta event. SMF stores no encoding, so the bytes
    // are identified by inspection - see the implementation for the measured
    // mix in real files and why the order is what it is.
    static QString decodeMetaText(const QByteArray& raw);

    // Song title straight out of the file, without loading it for playback -
    // the .mid counterpart of ImsPlayer/GybFileHandler::extractTitleQuick, used
    // by the playlist scanner and the title bar. Empty when the file carries no
    // title, which is most of them.
    static QString extractTitleQuick(const QString& fileName);

    unsigned long getCurrentTick() const; // Current playback position in MIDI ticks

    // 마커 채널 타이밍 추출
    struct MarkerEvent {
        unsigned long tick;        // 마커가 발생한 틱
        unsigned long timeMs;      // 마커가 발생한 시간 (밀리초)
        int noteNumber;            // 노트 번호 (마커 타입 구분용)
        int velocity;              // 벨로시티 (마커 정보)
    };
    QList<MarkerEvent> extractMarkerTimings(int channel = 11) const; // 기본값 11번 채널

    // Real-time key/tempo controls
    void setUserKeyTranspose(int key);
    int getUserKeyTranspose() const;
    void setUserTempoScale(int scale);
    int getUserTempoScale() const;
    int getCurrentBpm() const;

signals:
    void positionChanged(unsigned long position);
    void finished();
    void errorOccurred(const QString &error);

    // Channel monitor signals
    void noteOn(int channel, int note, int velocity);
    void noteOff(int channel, int note);
    void controllerChange(int channel, int controller, int value);
    void programChange(int channel, int program);
    void soundModeDetected(int mode); // 0=GM, 1=MT-32, 2=GS, 3=XG
    void soundModeReliabilityChanged(const SoundModeReliability& reliability);

private slots:
    void processEvents();
    void updateVolumeToDevice();
    void performSeek();

private:
    bool parseMidiFile(const QString &filename);
    unsigned long readVariableLength(std::istream &file);
    void resetPlayback();
    void sendMidiMessage(unsigned char status, unsigned char data1, unsigned char data2);
    void sendMidiMessage(DWORD message);
    void sendSysExMessage(const std::vector<unsigned char> &data);
    void initializeChannelState();
    void updateChannelState(unsigned char status, unsigned char data1, unsigned char data2);
    void sendCurrentChannelState();
    void performSeekImmediate(unsigned long position);
    unsigned long long ticksToMicroseconds(unsigned long ticks) const;

    // New real-time based functions
    unsigned long calculateCurrentTick(unsigned long long elapsedMs) const;
    double ticksToMilliseconds(unsigned long ticks, unsigned long tempo) const;
    unsigned long millisecondsToTicks(double ms, unsigned long tempo) const;
    void createGlobalEventList();
    void detectSoundMode();
    SoundModeReliability calculateSoundModeReliability();
    DetectionStrength getDetectionStrength(int confidenceScore);
    QString getConfusionHint(int detectedMode, int confidenceScore);
    int getMostLikelyAlternative(int detectedMode, int confidenceScore);

    HMIDIOUT hMidiOut;
    HANDLE m_sysExEvent;
    bool connected;
    bool playing;
    bool paused;

    QString currentFile;
    std::vector<MidiTrack> tracks;
    unsigned long ticksPerQuarter;
    unsigned long currentTempo; // microseconds per quarter note
    unsigned long startTime;
    qint64 pausedTime;

    // Real-time based playback variables
    qint64 playbackStartTime;  // Real time when playback started (ms)
    unsigned long currentTick;             // Current playback position in ticks
    std::vector<std::pair<unsigned long, unsigned long>> allEvents; // tick, eventIndex pairs
    unsigned long globalEventIndex;       // Current event index in sorted event list

    QElapsedTimer m_elapsedTimer;

    QMutex stateMutex; // Mutex to protect shared state between UI and playback threads
    PlaybackThread *playbackThread;

    QTimer *volumeUpdateTimer;
    QTimer *seekUpdateTimer;

    int currentVolume;
    unsigned long totalDuration;
    unsigned long pendingSeekPosition;

    // Track original channel volumes from MIDI file
    int originalChannelVolumes[16];  // Store original CC7 values from file

    // Tempo change tracking for accurate duration calculation
    struct TempoChange {
        unsigned long tick;     // Tick where tempo change occurs
        unsigned long timeUs;   // Accumulated microseconds at this point
        unsigned long tempo;    // New tempo in microseconds per quarter note
    };
    std::vector<TempoChange> tempoMap;

    // Current MIDI state for efficient seeking
    struct ChannelState {
        int volume;           // CC7
        int expression;       // CC11
        int program;          // Program Change
        int pitchBend;        // Pitch Bend
        int modWheel;         // CC1
        int sustain;          // CC64
        bool muted;           // Channel mute state
        // Add more controllers as needed
    };
    ChannelState currentChannelState[16];

    // NOB file support
    QTemporaryFile *currentTempFile = nullptr;  // Temporary MIDI file for NOB
    bool isNobFile = false;  // NOB 파일 여부 플래그
    bool m_isOkmFile = false; // .okm/.okw (Oksori via SoundFont) — slightly louder, attenuated

    // Internal Synth Support
    bool m_useInternalSynth = false;

    // Nuked-SC55 child process + named pipe (sc55/sc55bridge.h). Created lazily
    // the first time the user selects it, and kept alive while it remains the
    // chosen device.
    Sc55Bridge* m_pSc55 = nullptr;
    bool m_bUseSc55 = false;

    // munt, in this process (mt32synth.h). Created lazily like the SC-55 bridge
    // and kept while it remains the chosen device.
    Mt32Synth* m_pMt32 = nullptr;
    bool m_bUseMt32 = false;

    // Sound-module reset before each new song (midireset/midireset.h). Wired
    // to sendRawSysEx in the constructor; sent from play()'s new-song branch.
    MidiReset m_midiReset;
    QString m_currentSoundFontPath;

    int m_userKeyTranspose;
    int m_userTempoScale;
    int m_transposedNotes[16][128];
};

#endif // MIDIPLAYER_H
