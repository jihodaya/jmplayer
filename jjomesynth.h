#ifndef JJOMESYNTH_H
#define JJOMESYNTH_H

#include <QString>
#include <QMutex>
#include <atomic>
#include <thread>

// Forward declarations to avoid including heavy headers here
struct tsf;
struct ma_device;
struct ma_context;
struct ma_encoder;
class ImsPlayer;
class GybPlayer;
class OkaPlayer;

struct SynthEvent {
    enum Type { NoteOn, NoteOff, PitchBend, ControlChange, ProgramChange, SetVolume } type;
    int channel;
    int param1;
    int param2;
    float fparam;
};

class JJoMeSynth {
public:
    static JJoMeSynth& instance();

    ~JJoMeSynth();

    bool initialize(const QString& soundFontPath);
    void shutdown();
    bool isInitialized() const { return m_initialized.load(std::memory_order_relaxed); }

    void noteOn(int channel, int note, float velocity);
    void noteOff(int channel, int note);
    void pitchBend(int channel, int value);
    void controlChange(int channel, int control, int value);
    void programChange(int channel, int program);

    // Audio callback
    void renderAudio(void* output, unsigned int frameCount);

    bool startRecording(const QString& wavFilePath);
    void stopRecording();
    bool isRecording() const { return m_isRecording.load(std::memory_order_relaxed); }
    void setPlaybackActive(bool active) { m_isPlaybackActive.store(active, std::memory_order_relaxed); }

    void setVolume(float gain); // 0.0 to 1.0
    void setImsPlayer(class ImsPlayer* player);
    void setGybPlayer(class GybPlayer* player);
    void setOkaPlayer(class OkaPlayer* player);
    QString getSoundFontName() const;

    void setOplStereoMode(int mode);
    void forceApplyOplStereo();
    int getOplStereoMode() const { return m_oplStereoMode.load(std::memory_order_relaxed); }
    int getChannelPanBit(int ch) const {
        if (ch >= 0 && ch < 18) {
            return m_channelPanBits[ch].load(std::memory_order_relaxed);
        }
        return 0x00;
    }

private:
    JJoMeSynth(); // Singleton
    Q_DISABLE_COPY(JJoMeSynth)

    struct tsf* m_tsf;
    struct ma_device* m_device;
    struct ma_context* m_context;

    std::atomic<ImsPlayer*> m_imsPlayer;
    std::atomic<GybPlayer*> m_gybPlayer;
    std::atomic<OkaPlayer*> m_okaPlayer;
    std::atomic<bool> m_initialized;

    QString m_currentSoundFontPath;

    // For initialization and shutdown only
    QMutex m_mutex;

    // Recording members
    struct ma_encoder* m_encoder;
    std::atomic<bool> m_isRecording;
    std::atomic<bool> m_isPlaybackActive;
    QMutex m_encoderMutex;
    // Lazy WAV creation: the file is NOT created when recording is armed —
    // only when the first audio actually arrives (i.e. playback is running).
    // Guarded by m_encoderMutex.
    QString m_pendingWavPath;

    // Lock-free SPSC PCM Ring Buffer for recording
    // ~4 seconds of stereo float audio at 49716 Hz (≈1.5 MB)
    static const unsigned int PCM_RING_SAMPLES = 49716 * 2 * 4;
    float* m_pcmRing;
    std::atomic<unsigned int> m_pcmWritePos;
    std::atomic<unsigned int> m_pcmReadPos;

    // Dedicated recording writer thread (reads ring buffer → writes to disk)
    std::thread m_recThread;
    std::atomic<bool> m_recThreadRun;
    void recWriterLoop();
    void flushRemainingPcm();

    // Lock-free SPSC Ring Buffer for MIDI events
    static const int EVENT_QUEUE_SIZE = 1024;
    SynthEvent m_eventQueue[EVENT_QUEUE_SIZE];
    std::atomic<int> m_eventHead;
    std::atomic<int> m_eventTail;

    void pushEvent(const SynthEvent& ev);
    void processEvents();

    std::atomic<int> m_oplStereoMode;
    std::atomic<int> m_channelPanBits[18];
};

#endif // JJOMESYNTH_H
