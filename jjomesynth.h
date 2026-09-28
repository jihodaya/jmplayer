#ifndef JJOMESYNTH_H
#define JJOMESYNTH_H

#include <QString>
#include <QMutex>
#include <atomic>
#include <cstdint>
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
    enum Type { NoteOn, NoteOff, PitchBend, ControlChange, ProgramChange, SetVolume,
                SetDrumChannel } type;
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

    // Which channels are rhythm. GM says channel 10 and nothing else, and that
    // is the default here - but GS lets a file move the drums, and files do:
    // BAMBOO.RCP's control file puts them on channel 2 and leaves 10 melodic.
    // Without this the drum track came out as a pitched instrument.
    // For tests only: what the synthesiser currently has on a channel. There is
    // no other way to ask "did the seek actually land the right instrument", and
    // reading it back beats reasoning about the queue.
    int  debugChannelPreset(int channel) const;
    int  debugChannelBank(int channel) const;
    void debugDrainEvents();

    void setDrumChannel(int channel, bool isDrum);
    void resetDrumChannels();

    // Audio callback
    void renderAudio(void* output, unsigned int frameCount);

    bool startRecording(const QString& wavFilePath);
    // Returns the file that was written, or an empty string when no audio ever
    // reached the recorder - which is what happens when the song is going out
    // to an external MIDI device and this application renders nothing.
    QString stopRecording();
    QString recordingPath() const;
    bool isRecording() const { return m_isRecording.load(std::memory_order_relaxed); }
    void setPlaybackActive(bool active) { m_isPlaybackActive.store(active, std::memory_order_relaxed); }

    void setVolume(float gain); // 0.0 to 1.0
    void setImsPlayer(class ImsPlayer* player);
    void setGybPlayer(class GybPlayer* player);
    void setOkaPlayer(class OkaPlayer* player);

    // The X68000 MDX engine renders its own audio, like the OPL players.
    void setMdxPlayer(class MdxPlayer* player);
    // ADDITIVE, unlike every other player here - see mldfmplayer.h.
    void setMldFmPlayer(class MldFmPlayer* player);
    // The rate the device is actually open at. Anything that renders its own
    // audio has to run at this or it drifts - see mldfmplayer.cpp.
    int deviceSampleRate() const { return m_deviceSampleRate; }
    void setVgmPlayer(class VgmPlayer* player);

    // MT-32 (munt). Set while the MT-32 device is chosen; the render chain
    // below prefers it over the SoundFont, the same way the OPL players do.
    // Not owned - MidiPlayer keeps it alive and clears this before deleting.
    void setMt32Synth(class Mt32Synth* synth);
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
    std::atomic<class Mt32Synth*> m_mt32Synth{nullptr};
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
    QString m_writtenWavPath;   // set once the encoder is actually created

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

    std::atomic<class MdxPlayer*> m_mdxPlayer{nullptr};
    std::atomic<class MldFmPlayer*> m_mldFmPlayer{nullptr};
    int m_deviceSampleRate = 49716;
    std::atomic<class VgmPlayer*> m_vgmPlayer{nullptr};

    void pushEvent(const SynthEvent& ev);

    // Read and written only on the audio thread, inside processEvents().
    uint16_t m_drumChannels = (uint16_t)(1u << 9);   // GM default: channel 10
    int      m_lastProgram[16] = {0};
    bool isDrumChannel(int channel) const {
        return (m_drumChannels >> (channel & 15)) & 1;
    }
    void processEvents();

    std::atomic<int> m_oplStereoMode;
    std::atomic<int> m_channelPanBits[18];
};

#endif // JJOMESYNTH_H
