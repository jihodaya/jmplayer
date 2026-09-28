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
    bool isPaused() const { return paused.load(std::memory_order_relaxed); }

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
    // True once if loadMidiFile already told the user why it failed, so the
    // caller does not stack a generic box on top of a specific one.
    bool takeReportedError() { const bool r = m_reportedError; m_reportedError = false; return r; }
    bool m_reportedError = false;   // loadMidiFile already showed a message

    void setUserKeyTranspose(int key);
    int getUserKeyTranspose() const;
    void setUserTempoScale(int scale);
    int getUserTempoScale() const;
    int getCurrentBpm() const;

signals:
    void positionChanged(unsigned long position);
    // Emitted once per seek with the song position it landed on, whether
    // playing or paused. The MLD FM half follows it; the six transport paths
    // that seek all end in performSeekImmediate(), so this is the one place.
    void seeked(unsigned long positionMs);
    void finished();
    void errorOccurred(const QString &error);

    // Channel monitor signals
    void noteOn(int channel, int note, int velocity);
    void noteOff(int channel, int note);
    void controllerChange(int channel, int controller, int value);
    void programChange(int channel, int program);
    void soundModeDetected(int mode); // 0=GM, 1=MT-32, 2=GS, 3=XG
    void soundModeReliabilityChanged(const SoundModeReliability& reliability);

    // A song's opening SysEx is being handed to an external module. Only ever
    // emitted for a burst big enough to be a visible wait - see processEvents().
    void sysExTransfer(bool active, int kbSent);

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
    void applyGsRhythmSysEx(const std::vector<unsigned char> &data);
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
    // ATOMIC so the GUI can read them WITHOUT taking stateMutex.
    //
    // processEvents() holds stateMutex for as long as it takes to hand every
    // due event to the device, and sendSysExMessage() BLOCKS until the driver
    // has transmitted. The Prince of Persia MT-32 set puts 20,154 bytes of
    // timbre upload on tick 0 - 6.45 s on a 31250-baud link - and the whole
    // window froze for that time, because the position/time/BPM display calls
    // isPlaying(), getCurrentPosition() and getCurrentBpm() every tick and all
    // three used to wait on the same mutex. Reported as "it lags about five
    // seconds"; only external devices can show it, since munt and the internal
    // synth take a function call rather than a wire.
    //
    // Nothing here is read as a *group*, so a torn pair (position sampled either
    // side of a state change) is at worst one stale frame on a display. The
    // mutex still guards everything with structure - tracks, the event lists,
    // the device handles.
    std::atomic<bool> connected;
    std::atomic<bool> playing;
    std::atomic<bool> paused;

    QString currentFile;
    std::vector<MidiTrack> tracks;
    unsigned long ticksPerQuarter;
    std::atomic<unsigned long> currentTempo; // microseconds per quarter note
    unsigned long startTime;
    std::atomic<qint64> pausedTime;

    // Real-time based playback variables
    std::atomic<qint64> playbackStartTime;  // Real time when playback started (ms)
    std::atomic<unsigned long> currentTick;  // Current playback position in ticks
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

    // The whole controller map, per channel, so a seek can put the module back
    // exactly as the song left it. The six fields above cover CC1, 7, 11 and 64
    // and nothing else - not bank select, not pan, not reverb or chorus, not
    // the RPN/NRPN parameters - so seeking used to land on a channel whose
    // instrument was chosen from the wrong bank. 0xFF means "the song never
    // touched this one", and those are left alone rather than forced to a
    // default that might not be the module's.
    // How far through the song the SysEx have actually been handed to the
    // module. A seek only has to replay the ones beyond it - see the note in
    // performSeekImmediate.
    unsigned long m_sysExSentThroughTick = 0;
    // Bytes of SysEx handed over since the last ordinary MIDI event. A module
    // needs time to ACT on a bulk dump, not just to receive it - see the settle
    // in processEvents().
    qint64 m_sysExBurstBytes = 0;
    // Set once the song has played past tick 0. After that, a SysEx AT tick 0
    // can only be a re-visit - see processEvents().
    bool m_startupSysExDone = false;
    bool m_sysExTransferAnnounced = false;
    // How much slower than a MIDI cable to hand SysEx over, in percent of the
    // wire time. 100 = exactly cable speed. Settings key `Midi/SysExPacePct`;
    // see sendSysExMessage() for why a receiver may need more than 100.
    int m_sysExPacePct = 100;

public:
    void setSysExPacePct(int pct) { m_sysExPacePct = qBound(100, pct, 400); }
private:

    unsigned char m_ccState[16][128];
    // Which RPN / NRPN address is selected RIGHT NOW, so a data entry can be
    // filed against the right parameter. Only one of the two is ever live -
    // selecting an RPN deselects any NRPN and vice versa - which is the
    // hardware's own behaviour.
    int m_lastNrpn[16];     // (msb << 7) | lsb, or -1
    int m_lastRpn[16];

    // EVERY parameter the song has written, per channel, keyed by address.
    //
    // Keeping only the last one was a real defect: 6PONGI4 writes 6 to 20
    // parameters on every channel - vibrato rate/depth/delay, TVF cutoff and
    // resonance, envelope attack/decay/release, and 20 drum-note settings on
    // channel 10 - and a seek restored exactly one of them, with whatever value
    // CC6 happened to be holding. The channel came back with a different
    // timbre, which is what "seeking changes the instrument" turned out to be.
    // Invisible on the internal SoundFont, which ignores NRPN entirely.
    //
    // Value is (dataMSB << 8) | dataLSB, with 0x100 meaning "MSB only".
    QMap<int, int> m_nrpnValues[16];
    QMap<int, int> m_rpnValues[16];

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

    // Atomic for the same reason as the block above - the tempo/key readout
    // asks for these on the GUI thread every tick.
    std::atomic<int> m_userKeyTranspose;
    std::atomic<int> m_userTempoScale;
    int m_transposedNotes[16][128];
};

#endif // MIDIPLAYER_H
