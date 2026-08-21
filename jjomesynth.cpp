#include "jjomesynth.h"
#include <QDebug>
#include <QMutexLocker>
#include <QFileInfo>
#include <QDir>
#include "imsplayer.h"
#include "gybplayer.h"
#include "okaplayer.h"
#include "mt32synth.h"
#include "settingsmanager.h"
#include "opltunnelsender.h"

#include <cstring>
#include <chrono>
#include <algorithm>

#define TSF_IMPLEMENTATION
#include "tsf.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// miniaudio callback
static void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    Q_UNUSED(pInput);
    JJoMeSynth* synth = static_cast<JJoMeSynth*>(pDevice->pUserData);
    if (synth) {
        synth->renderAudio(pOutput, frameCount);
    }
}

JJoMeSynth& JJoMeSynth::instance() {
    static JJoMeSynth inst;
    return inst;
}

JJoMeSynth::JJoMeSynth()
    : m_tsf(nullptr)
    , m_device(nullptr)
    , m_context(nullptr)
    , m_imsPlayer(nullptr)
    , m_gybPlayer(nullptr)
    , m_okaPlayer(nullptr)
    , m_initialized(false)
    , m_encoder(nullptr)
    , m_isRecording(false)
    , m_isPlaybackActive(false)
    , m_pcmRing(nullptr)
    , m_pcmWritePos(0)
    , m_pcmReadPos(0)
    , m_recThreadRun(false)
    , m_eventHead(0)
    , m_eventTail(0)
    , m_oplStereoMode(1)
{
    for (int i = 0; i < 18; ++i) {
        m_channelPanBits[i].store(0x30, std::memory_order_relaxed); // Default is Mono (Center)
    }
}

JJoMeSynth::~JJoMeSynth() {
    shutdown();
}

bool JJoMeSynth::initialize(const QString& soundFontPath) {
    if (m_initialized.load(std::memory_order_relaxed)) {
        shutdown(); // Ensure clean slate if re-initialized
    }

    QMutexLocker locker(&m_mutex);
    qDebug() << "Initializing JJoMe Synth with SoundFont:" << soundFontPath;

    // Load SoundFont
#if defined(_WIN32)
    FILE* f = _wfopen(soundFontPath.toStdWString().c_str(), L"rb");
    if (f) {
        struct tsf_stream stream = { f, 
            [](void* data, void* ptr, unsigned int size) -> int { return (int)fread(ptr, 1, size, (FILE*)data); }, 
            [](void* data, unsigned int count) -> int { return !fseek((FILE*)data, count, SEEK_CUR); } 
        };
        m_tsf = tsf_load(&stream);
        fclose(f);
        if (m_tsf) {
            qDebug() << "SoundFont loaded successfully. Preset count:" << tsf_get_presetcount(m_tsf);
        } else {
            qWarning() << "tsf_load failed to parse SoundFont.";
        }
    } else {
        // An empty path is a request for the device alone (see below), not a
        // failure worth warning about.
        if (!soundFontPath.isEmpty())
            qWarning() << "Failed to open SoundFont file:" << soundFontPath;
        m_tsf = nullptr;
    }
#else
    m_tsf = tsf_load_filename(soundFontPath.toUtf8().constData());
#endif

    // An empty path means "open the device, no SoundFont".
    //
    // The MT-32 engine renders its own audio but still needs this device to
    // play it through, and a machine that has no .sf2 installed at all is
    // perfectly entitled to use it. Failing here would leave that user with a
    // working emulator and silence. Callers that do want a SoundFont still get
    // the old behaviour: a non-empty path that will not load is an error.
    if (!m_tsf && !soundFontPath.isEmpty()) {
        qWarning() << "Failed to load SoundFont:" << soundFontPath;
        return false;
    }

    // Set output mode to Stereo, Interleaved, 49716Hz
    if (m_tsf)
        tsf_set_output(m_tsf, TSF_STEREO_INTERLEAVED, 49716, 0);

    // Initialize miniaudio
    m_context = new ma_context;
    if (ma_context_init(NULL, 0, NULL, m_context) != MA_SUCCESS) {
        qWarning() << "Failed to initialize miniaudio context.";
        if (m_tsf) { tsf_close(m_tsf); m_tsf = nullptr; }
        delete m_context;
        m_context = nullptr;
        return false;
    }

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate        = 49716;
    deviceConfig.dataCallback      = audio_data_callback;
    deviceConfig.pUserData         = this;

    // Buffer depth. miniaudio's default period is about 10 ms, which is fine
    // until another program takes the CPU for longer than that - a game
    // loading, a virus scanner - and the callback misses its deadline, which
    // comes out as a click or a burst of noise rather than a pause. Asking for
    // a longer period costs latency nobody here can hear (the OPL tunnel is
    // clocked by its own sample counter, not by this callback) and buys
    // proportionally more slack. Settable because the right value depends on
    // the machine: Audio/BufferMs, 0 restores miniaudio's default.
    {
        int bufMs = SettingsManager::instance().value("Audio/BufferMs", 40).toInt();
        if (bufMs < 0)   bufMs = 0;
        if (bufMs > 500) bufMs = 500;
        if (bufMs > 0) {
            deviceConfig.periodSizeInMilliseconds = (ma_uint32) bufMs;
            deviceConfig.periods = 3;
        }
    }

    m_device = new ma_device;
    if (ma_device_init(m_context, &deviceConfig, m_device) != MA_SUCCESS) {
        qWarning() << "Failed to initialize audio device.";
        ma_context_uninit(m_context);
        delete m_context;
        delete m_device;
        m_context = nullptr;
        m_device = nullptr;
        if (m_tsf) { tsf_close(m_tsf); m_tsf = nullptr; }
        return false;
    }

    if (ma_device_start(m_device) != MA_SUCCESS) {
        qWarning() << "Failed to start audio device.";
        shutdown();
        return false;
    }

    m_initialized = true;
    m_currentSoundFontPath = soundFontPath;
    qDebug() << "JJoMe Synth initialized successfully.";
    return true;
}

void JJoMeSynth::shutdown() {
    stopRecording();
    bool wasInitialized = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized.load(std::memory_order_relaxed)) return;
        wasInitialized = m_initialized.load(std::memory_order_relaxed);
        m_initialized.store(false, std::memory_order_release);
    }

    // Clear event queue
    m_eventHead.store(0, std::memory_order_release);
    m_eventTail.store(0, std::memory_order_release);

    if (m_device) {
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
    }

    if (m_context) {
        ma_context_uninit(m_context);
        delete m_context;
        m_context = nullptr;
    }

    QMutexLocker locker(&m_mutex);
    if (m_tsf) {
        tsf_close(m_tsf);
        m_tsf = nullptr;
    }

    qDebug() << "JJoMe Synth shutdown completed.";
}

void JJoMeSynth::setImsPlayer(ImsPlayer* player) {
    m_imsPlayer.store(player, std::memory_order_release);
}

void JJoMeSynth::setGybPlayer(GybPlayer* player) {
    m_gybPlayer.store(player, std::memory_order_release);
}

void JJoMeSynth::setOkaPlayer(OkaPlayer* player) {
    m_okaPlayer.store(player, std::memory_order_release);
}

void JJoMeSynth::setMt32Synth(Mt32Synth* synth) {
    m_mt32Synth.store(synth, std::memory_order_release);
}

void JJoMeSynth::pushEvent(const SynthEvent& ev) {
    int currentTail = m_eventTail.load(std::memory_order_relaxed);
    int nextTail = (currentTail + 1) % EVENT_QUEUE_SIZE;
    if (nextTail != m_eventHead.load(std::memory_order_acquire)) {
        m_eventQueue[currentTail] = ev;
        m_eventTail.store(nextTail, std::memory_order_release);
    }
}

void JJoMeSynth::processEvents() {
    int currentHead = m_eventHead.load(std::memory_order_relaxed);
    int currentTail = m_eventTail.load(std::memory_order_acquire);
    while (currentHead != currentTail) {
        const SynthEvent& ev = m_eventQueue[currentHead];
        if (m_tsf) {
            switch (ev.type) {
                case SynthEvent::NoteOn:
                    tsf_channel_note_on(m_tsf, ev.channel, ev.param1, ev.fparam);
                    break;
                case SynthEvent::NoteOff:
                    tsf_channel_note_off(m_tsf, ev.channel, ev.param1);
                    break;
                case SynthEvent::PitchBend:
                    tsf_channel_set_pitchwheel(m_tsf, ev.channel, ev.param1);
                    break;
                case SynthEvent::ControlChange:
                    tsf_channel_midi_control(m_tsf, ev.channel, ev.param1, ev.param2);
                    break;
                case SynthEvent::ProgramChange:
                    tsf_channel_set_presetnumber(m_tsf, ev.channel, ev.param1, (ev.channel == 9));
                    break;
                case SynthEvent::SetVolume:
                    // Apply a slight attenuation factor (0.8x) to MIDI gain 
                    // to balance it with IMS/ROL (AdPlug) playback levels.
                    tsf_set_volume(m_tsf, ev.fparam * 0.8f);
                    break;
            }
        }
        currentHead = (currentHead + 1) % EVENT_QUEUE_SIZE;
    }
    m_eventHead.store(currentHead, std::memory_order_release);
}

void JJoMeSynth::renderAudio(void* output, unsigned int frameCount) {
    if (!m_initialized.load(std::memory_order_relaxed)) {
        memset(output, 0, frameCount * 2 * sizeof(float));
        return;
    }

    GybPlayer* gyb = m_gybPlayer.load(std::memory_order_acquire);
    ImsPlayer* ims = m_imsPlayer.load(std::memory_order_acquire);
    OkaPlayer* oka = m_okaPlayer.load(std::memory_order_acquire);
    Mt32Synth* mt32 = m_mt32Synth.load(std::memory_order_acquire);
    tsf* soundfont = m_tsf; // Assuming m_tsf is only changed during shutdown/init

    // Process queued MIDI events first
    processEvents();

    // Order matters, and it is not "most important first".
    //
    // The OPL players are a SOURCE; the MT-32 is a DESTINATION. They are not
    // alternatives competing for the same job: a .GYB playing through the OPL
    // engine makes its own sound and never touches the MIDI path, so it has to
    // win no matter which MIDI device happens to be selected. The MT-32 is only
    // the source when no OPL player is loaded - which is exactly the case where
    // a song is going out as MIDI, and is also what makes the same .GYB in MIDI
    // mode land here instead.
    //
    // Putting the MT-32 first, as this did when it was added, silences every
    // OPL song for anyone who has the MT-32 selected (reported 2026-08-21).
    // stop() clears the OPL pointers, so nothing lingers to block it afterwards.
    if (gyb) {
        bool playing = gyb->isPlaying();
        if (playing) {
            gyb->renderAudio(static_cast<float*>(output), frameCount);
        } else {
            // In GYB mode but not playing - should be silence
            memset(output, 0, frameCount * 2 * sizeof(float));
        }
    } else if (ims) {
        bool playing = ims->isPlaying();
        if (playing) {
            ims->renderAudio(static_cast<float*>(output), frameCount);
        } else {
            // In IMS mode but not playing - should be silence, not SoundFont fallback
            // This prevents SoundFont leakage during IMS track transitions
            memset(output, 0, frameCount * 2 * sizeof(float));
        }
    } else if (oka) {
        bool playing = oka->isPlaying();
        if (playing) {
            oka->renderAudio(static_cast<float*>(output), frameCount);
        } else {
            memset(output, 0, frameCount * 2 * sizeof(float));
        }
    } else if (mt32) {
        // No "not playing" branch, unlike the OPL players above: the MT-32
        // renders its own silence, and that silence includes the release tail
        // of whatever was sounding when the song stopped. Cutting it off with a
        // memset would clip the end of every note.
        mt32->Render(static_cast<float*>(output), frameCount);
    } else if (soundfont) {
        float* fOutput = static_cast<float*>(output);
        tsf_render_float(soundfont, fOutput, frameCount, 0);
    } else {
        memset(output, 0, frameCount * 2 * sizeof(float));
    }

    // Recording: copy PCM data to lock-free ring buffer (NO disk I/O here)
    if (m_isRecording.load(std::memory_order_acquire) && m_isPlaybackActive.load(std::memory_order_relaxed)) {
        if (m_pcmRing) {
            const float* src = static_cast<const float*>(output);
            unsigned int samplesToWrite = frameCount * 2; // stereo interleaved
            unsigned int writePos = m_pcmWritePos.load(std::memory_order_relaxed);
            unsigned int readPos  = m_pcmReadPos.load(std::memory_order_acquire);

            for (unsigned int i = 0; i < samplesToWrite; ++i) {
                unsigned int nextWrite = (writePos + 1) % PCM_RING_SAMPLES;
                if (nextWrite == readPos) break; // ring full — drop (should never happen with 4s buffer)
                m_pcmRing[writePos] = src[i];
                writePos = nextWrite;
            }
            m_pcmWritePos.store(writePos, std::memory_order_release);
        }
    }
}


void JJoMeSynth::setVolume(float gain) {
    pushEvent({SynthEvent::SetVolume, 0, 0, 0, gain});
    
    ImsPlayer* ims = m_imsPlayer.load(std::memory_order_acquire);
    if (ims) {
        ims->setVolume(static_cast<int>(gain * 100));
    }
    GybPlayer* gyb = m_gybPlayer.load(std::memory_order_acquire);
    if (gyb) {
        gyb->setVolume(static_cast<int>(gain * 100));
    }
    OkaPlayer* oka = m_okaPlayer.load(std::memory_order_acquire);
    if (oka) {
        oka->setVolume(static_cast<int>(gain * 100));
    }
}


void JJoMeSynth::noteOn(int channel, int note, float velocity) {
    pushEvent({SynthEvent::NoteOn, channel, note, 0, velocity});
}

void JJoMeSynth::noteOff(int channel, int note) {
    pushEvent({SynthEvent::NoteOff, channel, note, 0, 0.0f});
}

void JJoMeSynth::pitchBend(int channel, int value) {
    pushEvent({SynthEvent::PitchBend, channel, value, 0, 0.0f});
}

void JJoMeSynth::controlChange(int channel, int control, int value) {
    pushEvent({SynthEvent::ControlChange, channel, control, value, 0.0f});
}

void JJoMeSynth::programChange(int channel, int program) {
    pushEvent({SynthEvent::ProgramChange, channel, program, 0, 0.0f});
}

QString JJoMeSynth::getSoundFontName() const {
    if (!m_initialized.load(std::memory_order_relaxed) || m_currentSoundFontPath.isEmpty()) {
        return "";
    }
    return QFileInfo(m_currentSoundFontPath).fileName();
}

// ---------- Recording with lock-free ring buffer + writer thread ----------

bool JJoMeSynth::startRecording(const QString& wavFilePath) {
    QMutexLocker locker(&m_encoderMutex);
    if (m_isRecording.load(std::memory_order_relaxed)) return false;

    // Validate the target directory up front so obvious path errors still fail
    // here, but do NOT create the WAV file yet: recording is only ARMED now.
    // The file is created lazily by the writer thread when the first audio
    // arrives — audio only flows while playback is active (see renderAudio),
    // so pressing record without playing no longer leaves an empty WAV behind.
    QFileInfo fi(wavFilePath);
    QDir dir = fi.absoluteDir();
    if (!dir.exists() && !dir.mkpath(".")) return false;
    m_pendingWavPath = wavFilePath;
    m_encoder = nullptr;

    // Allocate PCM ring buffer (~1.5 MB)
    m_pcmRing = new float[PCM_RING_SAMPLES];
    m_pcmWritePos.store(0, std::memory_order_relaxed);
    m_pcmReadPos.store(0, std::memory_order_relaxed);

    // Start writer thread
    m_recThreadRun.store(true, std::memory_order_release);
    m_isRecording.store(true, std::memory_order_release);
    m_recThread = std::thread(&JJoMeSynth::recWriterLoop, this);

    return true;
}

void JJoMeSynth::stopRecording() {
    if (!m_isRecording.load(std::memory_order_relaxed)) return;

    // Signal the writer thread to stop and wait for it
    m_isRecording.store(false, std::memory_order_release);
    m_recThreadRun.store(false, std::memory_order_release);
    if (m_recThread.joinable()) {
        m_recThread.join();
    }

    // Flush any remaining data in the ring buffer
    flushRemainingPcm();

    // Clean up encoder. If it was never created (armed but nothing played),
    // no WAV file exists — exactly the desired behavior.
    QMutexLocker locker(&m_encoderMutex);
    m_pendingWavPath.clear();
    if (m_encoder) {
        ma_encoder_uninit(m_encoder);
        delete m_encoder;
        m_encoder = nullptr;
    }

    // Free ring buffer
    delete[] m_pcmRing;
    m_pcmRing = nullptr;
    m_pcmWritePos.store(0, std::memory_order_relaxed);
    m_pcmReadPos.store(0, std::memory_order_relaxed);
}

void JJoMeSynth::recWriterLoop() {
    // Local scratch buffer: 8192 samples = 4096 stereo frames ≈ 82ms at 49716Hz
    static const unsigned int CHUNK = 8192;
    float tempBuf[CHUNK];

    while (m_recThreadRun.load(std::memory_order_acquire)) {
        unsigned int readPos  = m_pcmReadPos.load(std::memory_order_relaxed);
        unsigned int writePos = m_pcmWritePos.load(std::memory_order_acquire);

        if (readPos == writePos) {
            // Nothing to write — sleep briefly to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Calculate available samples in ring buffer
        unsigned int available;
        if (writePos >= readPos) {
            available = writePos - readPos;
        } else {
            available = PCM_RING_SAMPLES - readPos + writePos;
        }

        unsigned int toRead = std::min(available, CHUNK);

        // Copy from ring buffer to local scratch
        for (unsigned int i = 0; i < toRead; ++i) {
            tempBuf[i] = m_pcmRing[readPos];
            readPos = (readPos + 1) % PCM_RING_SAMPLES;
        }
        m_pcmReadPos.store(readPos, std::memory_order_release);

        // Write to disk (this may block briefly — that's fine, we're on a dedicated thread)
        QMutexLocker locker(&m_encoderMutex);
        if (!m_encoder && !m_pendingWavPath.isEmpty()) {
            // First audio has arrived (playback is running) — create the WAV now.
            ma_encoder* enc = new ma_encoder;
            ma_encoder_config config = ma_encoder_config_init(
                ma_encoding_format_wav, ma_format_f32, 2, 49716
            );
            if (ma_encoder_init_file_w(m_pendingWavPath.toStdWString().c_str(),
                                       &config, enc) == MA_SUCCESS) {
                m_encoder = enc;
            } else {
                delete enc;
                qDebug() << "[JJoMeSynth] Failed to create WAV file:" << m_pendingWavPath;
            }
            m_pendingWavPath.clear();   // one attempt only
        }
        if (m_encoder) {
            ma_uint64 framesWritten;
            ma_encoder_write_pcm_frames(m_encoder, tempBuf, toRead / 2, &framesWritten);
        }
    }
}

void JJoMeSynth::flushRemainingPcm() {
    if (!m_pcmRing) return;

    static const unsigned int CHUNK = 8192;
    float tempBuf[CHUNK];

    unsigned int readPos  = m_pcmReadPos.load(std::memory_order_relaxed);
    unsigned int writePos = m_pcmWritePos.load(std::memory_order_acquire);

    while (readPos != writePos) {
        unsigned int available;
        if (writePos >= readPos) {
            available = writePos - readPos;
        } else {
            available = PCM_RING_SAMPLES - readPos + writePos;
        }

        unsigned int toRead = std::min(available, CHUNK);

        for (unsigned int i = 0; i < toRead; ++i) {
            tempBuf[i] = m_pcmRing[readPos];
            readPos = (readPos + 1) % PCM_RING_SAMPLES;
        }
        m_pcmReadPos.store(readPos, std::memory_order_release);

        QMutexLocker locker(&m_encoderMutex);
        if (m_encoder) {
            ma_uint64 framesWritten;
            ma_encoder_write_pcm_frames(m_encoder, tempBuf, toRead / 2, &framesWritten);
        }

        writePos = m_pcmWritePos.load(std::memory_order_acquire);
    }
}

void JJoMeSynth::setOplStereoMode(int mode) {
    if (mode < 1 || mode > 10) {
        mode = 1;
    }
    m_oplStereoMode.store(mode, std::memory_order_release);

    // OPL tunnel (2026-07-16): the receiver (mt32-pi fork) applies its OWN pan
    // policy to the ORIGINAL 0xC0 values jmp ships - tell it which of jmp's
    // virtual-stereo patterns to use so the Pi mix follows this setting.
    OplTunnelSender::instance().setStereoMode(mode);

    // 'L' -> 0x20 and 'R' -> 0x10 is the mirror of the chip's own bits
    // (nukedopl.c: 0x10 is `cha`, and `cha` is mixed into the LEFT output), so a
    // channel marked 'L' here is heard on the RIGHT. The jukebox and the mt32-pi
    // fork carry the same inversion, which is why every device agrees and the
    // only thing that was ever wrong is the printed letter - OplStereoDialog
    // therefore shows these patterns mirrored. Do not "fix" the bits without
    // changing all three together: it buys nothing audible and flips the stereo
    // image of every existing setting.
    // 1~9번에 대응하는 Panning 맵 정의
    static const char* MAPS[] = {
        "", // 인덱스 맞추기용 빈칸
        "MMMMMMMMMMM", // 1
        "MMRRLLMMMMR", // 2
        "LLLRRRMMMMR", // 3
        "LRLRLRMMMMR", // 4
        "RLRLRLMMMMR", // 5
        "LLLLRRRRMMR", // 6
        "RRRRLLLLMMR", // 7
        "RRRLLLRRRLR", // 8
        "LLRRLLRRLLR", // 9
        // 10, shown as [0]: GAYOBANG's own "스테레오". Not a pan pattern at
        // all - on the Oksori card ports 0x388 and 0x38A are two OPL chips,
        // one per speaker, and the setting plays the song on both with the
        // second detuned. This entry only places the first chip; the second is
        // added by the players' bank-1 mirror (see InterceptingOpl).
        "RRRRRRRRRRR"  // 10 - unused: the players place both chips themselves
    };

    const char* map = MAPS[mode];
    for (int i = 0; i < 18; ++i) {
        char p = map[i % 11];
        int val = 0x30; // M
        if (p == 'L') {
            val = 0x20;
        } else if (p == 'R') {
            val = 0x10;
        }
        m_channelPanBits[i].store(val, std::memory_order_release);
    }
}

void JJoMeSynth::forceApplyOplStereo() {
    ImsPlayer* ims = m_imsPlayer.load(std::memory_order_acquire);
    if (ims) {
        ims->forceUpdateOplStereo();
    }
    GybPlayer* gyb = m_gybPlayer.load(std::memory_order_acquire);
    if (gyb) {
        gyb->forceUpdateOplStereo();
    }
    OkaPlayer* oka = m_okaPlayer.load(std::memory_order_acquire);
    if (oka) {
        oka->forceUpdateOplStereo();
    }
}


