#include "midiplayer.h"
#include "gybokamidi.h"
#include "nobfilehandler.h"
#include "okafilehandler.h"
#include "opltunnelsender.h" // to stay off the wire while the OPL tunnel owns it
#include "settingsmanager.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDir>
#include <QCoreApplication>
#include <QRegularExpression>
#include <algorithm>


MidiPlayer::MidiPlayer(QObject *parent)
    : QObject(parent)
    , hMidiOut(nullptr)
    , m_sysExEvent(nullptr)
    , connected(false)
    , playing(false)
    , paused(false)
    , ticksPerQuarter(480)
    , currentTempo(500000) // Default: 120 BPM
    , startTime(0)
    , pausedTime(0)
    , currentVolume(96)
    , totalDuration(0)
    , m_userKeyTranspose(0)
    , m_userTempoScale(100)
{
    // Enable high-resolution precision timer for Windows MIDI synchronization
    timeBeginPeriod(1);
    m_elapsedTimer.start();

    // Sound-module reset before each new song (midireset/midireset.h): route
    // its SysEx through our own device send, and let it pace multi-message
    // resets with a real sleep (only matters for slow hardware synths).
    m_midiReset.SetSendCallback([this](const std::vector<uint8_t>& data) {
        sendSysExMessage(data);
    });
    m_midiReset.SetSleepFunction([](unsigned ms) { QThread::msleep(ms); });

    // Create and configure playback thread
    playbackThread = new PlaybackThread(this);

    volumeUpdateTimer = new QTimer(this);
    connect(volumeUpdateTimer, &QTimer::timeout, this, &MidiPlayer::updateVolumeToDevice);
    volumeUpdateTimer->setSingleShot(true); // Only fire once
    volumeUpdateTimer->setInterval(100); // Wait 100ms after volume change stops

    seekUpdateTimer = new QTimer(this);
    connect(seekUpdateTimer, &QTimer::timeout, this, &MidiPlayer::performSeek);
    seekUpdateTimer->setSingleShot(true); // Only fire once
    seekUpdateTimer->setInterval(150); // Wait 150ms after seek change stops

    // Initialize channel volumes to default
    for (int i = 0; i < 16; i++) {
        originalChannelVolumes[i] = 100; // Default MIDI volume
    }

    initializeChannelState();
}

MidiPlayer::~MidiPlayer()
{
    disconnect();
    if (playbackThread) {
        playbackThread->stop();
        playbackThread->wait();
        delete playbackThread;
    }

    // Sc55Bridge is a QObject child and goes with the parent; the MT-32 engine
    // is not, so it is deleted here. disconnect() has already unpublished it
    // from the audio callback.
    if (m_pMt32) {
        JJoMeSynth::instance().setMt32Synth(nullptr);
        delete m_pMt32;
        m_pMt32 = nullptr;
    }

    timeEndPeriod(1);
}

QStringList MidiPlayer::getAvailableDevices()
{
    QStringList devices;

    // Maximum error suppression during device enumeration
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT);
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT, NULL);
    SetUnhandledExceptionFilter(NULL);

    UINT numDevices = midiOutGetNumDevs();

    for (UINT i = 0; i < numDevices; i++) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            devices.append(QString::fromWCharArray(caps.szPname));
        }
    }

    // Keep error suppression active globally

    return devices;
}

bool MidiPlayer::connectToDevice(int deviceId)
{
    if (connected) {
        disconnect(false);
    }

    if (deviceId == -1) {
        m_useInternalSynth = true;
        connected = true;
        
        // Initialize all channels to default volume
        for (int channel = 0; channel < 16; channel++) {
            originalChannelVolumes[channel] = 100; // Reset to default
        }
        
        return true;
    }

    m_useInternalSynth = false;

    // Check if device ID is valid first
    UINT numDevices = midiOutGetNumDevs();
    if (deviceId < 0 || (UINT)deviceId >= numDevices) {
        return false;
    }

    // Check device capabilities before attempting connection
    MIDIOUTCAPS caps;
    if (midiOutGetDevCaps(deviceId, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
        return false;
    }

    // Disable Windows error reporting completely
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT);
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT, NULL);

    // Disable application error reporting
    SetUnhandledExceptionFilter(NULL);

    if (!m_sysExEvent) {
        m_sysExEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    MMRESULT result = midiOutOpen(&hMidiOut, deviceId, (DWORD_PTR)m_sysExEvent, 0, CALLBACK_EVENT);

    if (result == MMSYSERR_NOERROR) {
        connected = true;

        // Verification log: which device actually got opened (caps fetched above)
        qDebug() << "[MidiPlayer] Connected to MIDI device" << deviceId
                 << QString::fromWCharArray(caps.szPname);

        // Initialize all channels to default volume
        for (int channel = 0; channel < 16; channel++) {
            originalChannelVolumes[channel] = 100; // Reset to default
        }

        return true;
    }

    qWarning() << "[MidiPlayer] midiOutOpen failed for device" << deviceId
               << QString::fromWCharArray(caps.szPname) << "error" << result;
    return false;
}

// Nuked-SC55 as a destination. Launches the emulator and takes over the send
// path; see sc55/sc55bridge.h for why a pipe rather than a virtual MIDI cable.
bool MidiPlayer::connectToSc55()
{
    if (connected)
        disconnect(false);

    if (!m_pSc55) {
        m_pSc55 = new Sc55Bridge(this);
        connect(m_pSc55, &Sc55Bridge::emulatorStopped, this, [this]() {
            // The emulator went away by itself. Drop the connection so the next
            // note doesn't vanish into nothing.
            m_bUseSc55 = false;
            connected = false;
        });
    }

    if (!m_pSc55->Start())
        return false;

    m_useInternalSynth = false;
    m_bUseSc55 = true;
    connected = true;

    for (int channel = 0; channel < 16; channel++)
        originalChannelVolumes[channel] = 100;

    qDebug() << "[MidiPlayer] Connected to Nuked-SC55 over the named pipe";
    return true;
}

// MT-32 / CM-32L through munt. Nothing is launched and nothing is connected to:
// the emulator is a library in this process, so "connecting" means loading a
// ROM set and telling JJoMeSynth to render it.
//
// The audio device has to be up for anything to be heard, and it belongs to
// JJoMeSynth. It is opened with an empty SoundFont path on purpose - this
// engine makes its own sound, and a machine with no .sf2 at all is still
// entitled to play MT-32 files.
bool MidiPlayer::connectToMt32()
{
    if (connected)
        disconnect(false);

    if (!m_pMt32)
        m_pMt32 = new Mt32Synth();

    const QString saved = SettingsManager::instance().value("Mt32/RomSet", "").toString();
    if (!m_pMt32->Open(saved)) {
        qWarning() << "[MidiPlayer] MT-32 failed to open:" << m_pMt32->ErrorString();
        return false;
    }

    if (!JJoMeSynth::instance().isInitialized()) {
        if (!JJoMeSynth::instance().initialize(QString())) {
            qWarning() << "[MidiPlayer] MT-32: no audio device";
            m_pMt32->Close();
            return false;
        }
    }

    // Published last, so the audio callback never sees a half-built synth.
    JJoMeSynth::instance().setMt32Synth(m_pMt32);

    m_useInternalSynth = false;
    m_bUseMt32 = true;
    connected  = true;

    for (int channel = 0; channel < 16; channel++)
        originalChannelVolumes[channel] = 100;

    // Start the panel in agreement with the slider rather than at the
    // machine's power-on default.
    m_pMt32->SetMasterVolume(currentVolume);

    qDebug() << "[MidiPlayer] Connected to the MT-32 engine:" << m_pMt32->CurrentRomLabel();
    return true;
}

bool MidiPlayer::connectToDeviceByName(const QString& deviceName)
{
    // Look the device up by name in the CURRENT enumeration instead of trusting
    // a combo-box index captured at startup. With Windows 11's new MIDI stack
    // (Windows MIDI Services / midisrv active), WinMM device order can change
    // between app start and connect time, so an index snapshot may open a
    // different device (typically Microsoft GS Wavetable Synth -> PC speakers)
    // than the one the user picked.
    UINT numDevices = midiOutGetNumDevs();
    for (UINT i = 0; i < numDevices; i++) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            if (QString::fromWCharArray(caps.szPname) == deviceName) {
                return connectToDevice((int)i);
            }
        }
    }
    qWarning() << "[MidiPlayer] MIDI device not found by name:" << deviceName;
    return false;
}

void MidiPlayer::disconnect(bool async)
{
    // Nuked-SC55 has no WinMM handle to clean up - just silence it and shut the
    // emulator down. Done before the lock: the bridge terminates a child
    // process, which has nothing to do with the playback thread's state.
    if (m_bUseSc55 && m_pSc55) {
        if (connected) {
            for (int channel = 0; channel < 16; channel++) {
                m_pSc55->SendShort(0xB0 | channel, 0x7B, 0); // all notes off
                m_pSc55->SendShort(0xB0 | channel, 0x78, 0); // all sound off
            }
        }
        m_pSc55->Stop();
        m_bUseSc55 = false;
        connected = false;
        return;
    }

    // Same shape for the MT-32, and for the same reason: no WinMM handle to
    // clean up. Unpublish it from the audio callback BEFORE closing, or the
    // render thread can be inside Render() while the synth goes away.
    if (m_bUseMt32 && m_pMt32) {
        if (connected)
            m_pMt32->AllSoundOff();
        JJoMeSynth::instance().setMt32Synth(nullptr);
        m_pMt32->Close();
        m_bUseMt32 = false;
        connected = false;
        return;
    }

    HMIDIOUT localHMidiOut = nullptr;
    HANDLE localSysExEvent = nullptr;

    // Ensure playback thread is not accessing MIDI device
    {
        QMutexLocker locker(&stateMutex);
        if (playing) {
            playing = false;
            paused = false;
        }
        
        if (connected) {
            // Send all notes off (safe since we stopped playing)
            for (int channel = 0; channel < 16; channel++) {
                sendMidiMessage(0xB0 | channel, 0x7B, 0);
                sendMidiMessage(0xB0 | channel, 0x78, 0);
            }
            resetPlayback();
            
            if (!m_useInternalSynth) {
                localHMidiOut = hMidiOut;
            }
            hMidiOut = nullptr; // Clear member instantly to prevent background thread access
            
            if (m_sysExEvent) {
                localSysExEvent = m_sysExEvent;
            }
            m_sysExEvent = nullptr;

            connected = false;
        }
    }

    // Perform heavy Windows Multimedia API cleanup.
    // Asynchronously in a background thread to prevent UI freezing (kernel lock contention) with miniaudio's device stream,
    // OR synchronously when changing devices to avoid MMSYSERR_ALLOCATED.
    if (localHMidiOut) {
        if (async) {
            QThread* cleanupThread = QThread::create([localHMidiOut]() {
                midiOutReset(localHMidiOut);
                QThread::msleep(50);
                midiOutClose(localHMidiOut);
            });
            connect(cleanupThread, &QThread::finished, cleanupThread, &QObject::deleteLater);
            cleanupThread->start();
        } else {
            midiOutReset(localHMidiOut);
            midiOutClose(localHMidiOut); // Sync close to free device immediately
        }
    }

    if (localSysExEvent) {
        CloseHandle(localSysExEvent);
    }
}

bool MidiPlayer::isConnected() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    return connected;
}

void MidiPlayer::sendLiveProgramChange(int channel, int bankMsb, int program)
{
    if (channel < 0 || channel > 15) return;
    if (program < 0 || program > 127) return;
    if (bankMsb > 0)
        sendMidiMessage((unsigned char)(0xB0 | channel), 0, (unsigned char)bankMsb);
    sendMidiMessage((unsigned char)(0xC0 | channel), (unsigned char)program, 0);
}

bool MidiPlayer::loadMidiFile(const QString &filename)
{
    // Stop playback before modifying tracks to prevent background thread crash
    stop();
    
    QString actualFilename = filename;
    QString tempMidiPath;

    // A .GYB or .OKA reaches this function only when the song is set to play
    // through MIDI rather than OPL (see MainWindow::playsViaMidi). Build the
    // stream from the song's notes and its instrument assignment, then hand it
    // on exactly like a .NOB. .OKM and .OKW are not included: they already play
    // as MIDI, and routing them through the GM remap would change how songs
    // that have always worked sound.
    if (gybokamidi::isSupported(filename)) {
        QString err;
        QByteArray midiData = gybokamidi::toMidi(
            filename, gybokamidi::buildPlan(filename), &err,
            gybokamidi::targetModule(filename));
        if (midiData.isEmpty()) {
            qWarning() << "[MidiPlayer] MIDI build failed for" << filename << err;
            emit errorOccurred(err.isEmpty() ? QString("Could not build MIDI") : err);
            return false;
        }

        QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
        QDir().mkpath(tempDir);
        QTemporaryFile *tempFile = new QTemporaryFile(tempDir + "/opl_XXXXXX.mid", this);
        if (!tempFile->open()) {
            emit errorOccurred("Failed to create temporary MIDI file");
            delete tempFile;
            return false;
        }
        tempFile->write(midiData);
        tempFile->flush();
        tempMidiPath = tempFile->fileName();
        actualFilename = tempMidiPath;
        if (currentTempFile) delete currentTempFile;
        currentTempFile = tempFile;
        qDebug() << "[MidiPlayer] built" << midiData.size() << "bytes of MIDI from" << filename;
    }
    // NOB 파일인 경우 MIDI 데이터 추출
    else if (filename.toLower().endsWith(".nob")) {
        qDebug() << "NOB file detected:" << filename;

        // NOB 파일 검증
        if (!NobFileHandler::isNobFile(filename)) {
            qWarning() << "Invalid NOB file format:" << filename;
            emit errorOccurred("Invalid NOB file format");
            return false;
        }

        // MIDI 데이터 추출
        QByteArray midiData = NobFileHandler::extractMidiData(filename);
        if (midiData.isEmpty()) {
            qWarning() << "Failed to extract MIDI data from NOB file:" << filename;
            emit errorOccurred("Failed to extract MIDI data from NOB file");
            return false;
        }

        // 임시 MIDI 파일 생성
        QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
        QDir().mkpath(tempDir);
        QTemporaryFile *tempFile = new QTemporaryFile(tempDir + "/nob_XXXXXX.mid", this);
        if (!tempFile->open()) {
            qWarning() << "Failed to create temporary MIDI file";
            emit errorOccurred("Failed to create temporary MIDI file");
            delete tempFile;
            return false;
        }

        tempFile->write(midiData);
        tempFile->flush();

        tempMidiPath = tempFile->fileName();
        actualFilename = tempMidiPath;

        qDebug() << "Created temporary MIDI file:" << tempMidiPath;

        // Keep the temp file object alive (will be cleaned up in destructor or next load)
        if (currentTempFile) {
            delete currentTempFile;
        }
        currentTempFile = tempFile;
    }
    // OKA/OKM/OKW (Oksori Music File): XOR 0xA8 decode → standard MIDI, then
    // play exactly like NOB. The music IS a standard SMF once decoded.
    else if (filename.toLower().endsWith(".oka") ||
             filename.toLower().endsWith(".okm") ||
             filename.toLower().endsWith(".okw")) {
        qDebug() << "OKA/OKM file detected:" << filename;

        if (!OkaFileHandler::isOkaFile(filename)) {
            qWarning() << "Invalid OKA file format:" << filename;
            emit errorOccurred("Invalid OKA file format");
            return false;
        }

        QByteArray midiData = OkaFileHandler::extractMidiData(filename);
        if (midiData.isEmpty()) {
            qWarning() << "Failed to extract MIDI data from OKA file:" << filename;
            emit errorOccurred("Failed to extract MIDI data from OKA file");
            return false;
        }

        QString tempDir = QCoreApplication::applicationDirPath() + "/temp";
        QDir().mkpath(tempDir);
        QTemporaryFile *tempFile = new QTemporaryFile(tempDir + "/oka_XXXXXX.mid", this);
        if (!tempFile->open()) {
            qWarning() << "Failed to create temporary MIDI file";
            emit errorOccurred("Failed to create temporary MIDI file");
            delete tempFile;
            return false;
        }

        tempFile->write(midiData);
        tempFile->flush();

        tempMidiPath = tempFile->fileName();
        actualFilename = tempMidiPath;

        qDebug() << "Created temporary MIDI file (OKA):" << tempMidiPath;

        if (currentTempFile) {
            delete currentTempFile;
        }
        currentTempFile = tempFile;
    }

    if (!parseMidiFile(actualFilename)) {
        return false;
    }

    currentFile = filename;  // Store original NOB filename
    {
        QString lf = filename.toLower();
        m_isOkmFile = lf.endsWith(".okm") || lf.endsWith(".okw");
    }
    resetPlayback();

    // Detect sound mode after loading MIDI file
    detectSoundMode();

    return true;
}

void MidiPlayer::setIsNobFile(bool isNob)
{
    isNobFile = isNob;
    qDebug() << "[MidiPlayer] File type:" << (isNob ? "NOB" : "Standard MIDI");
}

void MidiPlayer::play()
{
    QMutexLocker locker(&stateMutex);
    
    if (!connected || tracks.empty()) return;

    if (paused) {
        playbackStartTime = m_elapsedTimer.elapsed() - (pausedTime / (m_userTempoScale / 100.0));
        paused = false;
    } else {
        resetPlayback();
        currentTick = 0;

        // Reset the external sound module BEFORE this song's own events start,
        // so no leftover instrument/pan/reverb/tuning state from the previous
        // song bleeds into it (midireset/midireset.h; no-op when disabled or
        // when playing to the internal synth). Only for a genuine new-song
        // start (the paused branch above skips it - a resume must not wipe the
        // state the paused song had already established).
        if (!m_useInternalSynth)
            m_midiReset.SendResets();

        // Reset channel state including mute settings
        initializeChannelState();

        // Reset channel volumes to default at start
        for (int channel = 0; channel < 16; channel++) {
            originalChannelVolumes[channel] = 100;
        }

        // Start the clock only NOW, after every pre-roll message is out.
        //
        // It used to be taken before SendResets(), which blocks until the
        // driver reports each SysEx as sent (and, with a settle delay
        // configured, sleeps on top of that). All of that time was then
        // counted as song time already elapsed, so the playback thread woke up
        // "behind" and fired everything scheduled inside that window at once -
        // the song's opening was compressed and the beat came out wrong, but
        // only with the reset feature switched on. Reported by a user
        // (알로에, 2026-07-27) on a file whose intro is sparse enough that the
        // displaced first notes are plainly audible; a busy intro hides it,
        // which is why it looked file-specific.
        playbackStartTime = m_elapsedTimer.elapsed();
    }

    playing = true;
    
    if (!playbackThread->isRunning()) {
        playbackThread->start(QThread::HighPriority);
    }
}

void MidiPlayer::pause()
{
    QMutexLocker locker(&stateMutex);
    if (playing) {
        playing = false;
        paused = true;
        pausedTime = (m_elapsedTimer.elapsed() - playbackStartTime) * (m_userTempoScale / 100.0);
        // The background thread will loop harmlessly when NOT playing (but will yield to CPU)

        // Stop all currently playing notes
        for (int channel = 0; channel < 16; channel++) {
            sendMidiMessage(0xB0 | channel, 0x7B, 0); // All notes off
            sendMidiMessage(0xB0 | channel, 0x78, 0); // All sound off
        }
    }
}

void MidiPlayer::stop()
{
    QMutexLocker locker(&stateMutex);
    playing = false;
    paused = false;
    // Don't kill the thread, just change the state

    // Send all notes off
    for (int channel = 0; channel < 16; channel++) {
        sendMidiMessage(0xB0 | channel, 0x7B, 0); // All notes off
        sendMidiMessage(0xB0 | channel, 0x78, 0); // All sound off
    }

    resetPlayback();
}

bool MidiPlayer::isPlaying() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    return playing;
}

void MidiPlayer::setVolume(int volume)
{
    QMutexLocker locker(&stateMutex);
    currentVolume = qBound(0, volume, 127);

    // m_bUseSc55 included: the emulator has no hMidiOut, so it used to be left
    // out of the volume update entirely and the slider did nothing (2026-07-29).
    if (connected && (hMidiOut || m_useInternalSynth || m_bUseSc55 || m_bUseMt32)) {
        // Restart timer - this will delay the actual update until user stops moving slider
        locker.unlock(); // unlock before QTimer operation (QTimer must run on GUI thread)
        volumeUpdateTimer->start();
    }
}

int MidiPlayer::getVolume() const
{
    return currentVolume;
}

QString MidiPlayer::getCurrentFile() const
{
    return currentFile;
}

unsigned long MidiPlayer::getCurrentPosition() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    if (!playing && !paused) return 0;

    unsigned long currentTime;
    if (playing) {
        currentTime = (m_elapsedTimer.elapsed() - playbackStartTime) * (m_userTempoScale / 100.0);
    } else {
        currentTime = pausedTime;
    }

    return currentTime;
}

unsigned long MidiPlayer::getTotalDuration() const
{
    return totalDuration; // Set during parsing, thread-safe to read
}

unsigned long MidiPlayer::getCurrentTick() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    return currentTick;
}

unsigned long MidiPlayer::getTotalTicks() const
{
    // 마지막 이벤트의 틱 수 반환
    if (allEvents.empty()) {
        return 0;
    }
    return allEvents.back().first; // (tick, eventIndex) 쌍에서 tick
}

void MidiPlayer::setPosition(unsigned long position)
{
    if (position <= totalDuration) {
        // Perform seek immediately without any delay
        performSeekImmediate(position);
    }
}

QString MidiPlayer::getTrackInfo() const
{
    if (tracks.empty()) return QString();

    int totalEvents = 0;
    for (const auto& track : tracks) {
        totalEvents += track.events.size();
    }

    return QString("Tracks: %1 | Events: %2").arg(tracks.size()).arg(totalEvents);
}

QList<unsigned long> MidiPlayer::extractLyricSyllableTicks() const
{
    // A karaoke MIDI timestamps every lyric event, so the syllable timing is in
    // the file - no counting notes, no guessing which channel is the vocal, no
    // trimming an intro. Same quality of information OKM carries, and the reason
    // this path can be exact where NOB needs heuristics.
    //
    // One tick per SUNG syllable, matching how the highlight is advanced
    // (mainwindow_playback.cpp indexes syllables, not events), so the sustain
    // marks and line breaks that extractLyrics() drops are skipped here too.
    QList<unsigned long> ticks;

    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (!event.isMetaEvent || event.metaData.empty()) continue;
            if (event.data1 != 0x05 && event.data1 != 0x01) continue;

            QByteArray payload(reinterpret_cast<const char*>(event.metaData.data()),
                               event.metaData.size());
            const QString text = decodeMetaText(payload);

            // One tick per DISPLAYED character, because that is what the
            // highlight steps through. Most events carry a single syllable, but
            // English words arrive whole ("everybody^"), so counting one tick per
            // event would drift. Bracketed markers like <간주> / <inst> are
            // interlude cues, not lyrics - extractLyrics() shows them, yet they
            // are never sung, so they get a tick each and no more.
            int shown = 0;
            for (const QChar& c : text) {
                if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) continue;
                if (c == QLatin1Char('^') || c == QLatin1Char('-') ||
                    c == QLatin1Char('@') || c.isSpace()) continue;
                shown++;
            }
            for (int i = 0; i < shown; ++i)
                ticks.append(static_cast<unsigned long>(event.absoluteTimeUs));
        }
    }

    std::sort(ticks.begin(), ticks.end());
    qDebug() << "[MidiPlayer] Extracted" << ticks.size() << "lyric syllable ticks";
    return ticks;
}

// Names that occupy the title slot without saying anything. Measured over the
// local library: 57 files are left at the sequencer's "untitled" default and 31
// carry the "WinJammer Demo" watermark, and in every one of those cases the
// filename is the more informative of the two - several are Korean song titles.
static bool isPlaceholderTitle(const QString& title)
{
    static const QStringList kPlaceholders = {
        "untitled", "unnamed", "winjammer demo"
    };
    if (kPlaceholders.contains(title.toLower())) return true;

    // Nothing but punctuation ("??" and friends) is no better.
    for (const QChar& c : title) {
        if (c.isLetterOrNumber()) return false;
    }
    return true;
}

// Song title, read straight from the file without loading it for playback.
//
// SMF has no dedicated title field the way NOB/GYB/IMS headers do; the
// convention is that track 0 (the tempo track of a format-1 file) carries the
// song name in a Sequence/Track Name meta event, while the remaining tracks name
// their instruments. Only track 0 is trusted here - measured over the local
// library, reading every track instead would return "melody", "bass", "Kick"
// for the karaoke files, which is worse than showing the filename.
//
// KAR files instead put "@T<title>" in a text event, keyed by a "@K" magic in
// the same track; that is honoured when the magic is present so the text cannot
// be mistaken for an ordinary comment.
QString MidiPlayer::extractTitleQuick(const QString& fileName)
{
    QFile f(fileName);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QByteArray data = f.read(1 << 20);   // a title lives near the top
    f.close();

    const int n = data.size();
    if (n < 14 || data.left(4) != "MThd") return QString();

    auto be32 = [&data](int at) -> quint32 {
        return (quint32(quint8(data[at]))     << 24) |
               (quint32(quint8(data[at + 1])) << 16) |
               (quint32(quint8(data[at + 2])) <<  8) |
                quint32(quint8(data[at + 3]));
    };

    int pos = 8 + int(be32(4));
    int track = 0;
    QByteArray trackName;      // track 0 only
    QByteArray karTitle;
    bool karMagic = false;

    while (pos + 8 <= n && data.mid(pos, 4) == "MTrk") {
        const int len = int(be32(pos + 4));
        int p = pos + 8;
        const int end = qMin(p + len, n);      // truncated files are common
        quint8 running = 0;

        while (p < end) {
            while (p < end && (quint8(data[p]) & 0x80)) p++;   // delta time
            if (p >= end) break;
            p++;

            if (p >= end) break;
            quint8 status = quint8(data[p]);
            if (status & 0x80) p++; else status = running;
            if (status & 0x80 && status < 0xF0) running = status;

            if (status == 0xFF) {
                if (p >= end) break;
                const quint8 type = quint8(data[p++]);
                int len2 = 0;
                while (p < end) {
                    const quint8 b = quint8(data[p++]);
                    len2 = (len2 << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (len2 < 0 || p + len2 > end) break;
                const QByteArray payload = data.mid(p, len2);
                p += len2;

                if (type == 0x03 && track == 0 && trackName.isEmpty())
                    trackName = payload;
                else if (type == 0x01) {
                    if (payload.startsWith("@K")) karMagic = true;
                    else if (payload.startsWith("@T") && karTitle.isEmpty())
                        karTitle = payload.mid(2);
                }
            } else if (status == 0xF0 || status == 0xF7) {
                int len2 = 0;
                while (p < end) {
                    const quint8 b = quint8(data[p++]);
                    len2 = (len2 << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (len2 < 0 || p + len2 > end) break;
                p += len2;
            } else if (status >= 0x80) {
                p += (status >= 0xC0 && status <= 0xDF) ? 1 : 2;
            } else {
                break;   // no running status yet - give up on this track
            }
        }

        pos = qMin(pos + 8 + len, n);
        track++;
    }

    const QByteArray raw = (karMagic && !karTitle.isEmpty()) ? karTitle : trackName;
    QString title = decodeMetaText(raw).trimmed();

    // Same guard the IMS path uses: a "title" that just repeats the filename
    // tells the user nothing.
    if (title.isEmpty() || isPlaceholderTitle(title) ||
        title == QFileInfo(fileName).baseName()) {
        return QString();
    }
    return title;
}

QString MidiPlayer::decodeMetaText(const QByteArray& raw)
{
    // Standard MIDI carries no encoding information in its text events, so the
    // bytes have to be identified by inspection. Measured over 32,964 text
    // events in the local library (2026-07-31):
    //
    //   ASCII    13,056   plain English titles
    //   CP949    19,423   Korean lyrics - the bulk of it
    //   UTF-8       363
    //   Shift-JIS       2   Japanese titles (Final Fantasy rips)
    //   Latin-1       120   the (c) sign in copyright lines
    //
    // NOT ONE file decoded as UTF-8 alone, which is what the old code assumed -
    // so every Korean lyric came out as replacement characters.
    //
    // Order matters. UTF-8 first because its multi-byte form is strict enough
    // that a false positive is unlikely; then CP949 and Shift-JIS, each accepted
    // only if it yields characters from that language rather than merely
    // decoding; Latin-1 last as the catch-all, since it accepts any byte and so
    // can never fail - which is what stops '(c)' turning into a broken glyph.
    if (raw.isEmpty()) return QString();

    bool ascii = true;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x80) { ascii = false; break; }
    }
    if (ascii) return QString::fromLatin1(raw);

    {
        auto toUtf8 = QStringDecoder(QStringDecoder::Utf8);
        const QString s = toUtf8(raw);
        if (!toUtf8.hasError()) return s;
    }

    // Legacy codepages go through the Windows API, not QStringDecoder: Qt6 only
    // builds in the UTF family plus Latin-1, and this Qt has no ICU, so
    // QStringDecoder("CP949") is simply invalid and every Korean lyric fell
    // through to Latin-1 - which is why they showed up as "¾Æ" instead of "아".
    // The rest of the player already decodes Korean this way (NobFileHandler).
    // MB_ERR_INVALID_CHARS matters more than it looks. Without it the API never
    // fails - it quietly substitutes whatever it cannot map - so CP949 "succeeded"
    // on Shift-JIS bytes and, because a few of those pairs happen to land on real
    // Hangul, passed the check below. That is how 999_GS2.MID's Japanese title
    // came out as '뗢됋밪벞괱괱괱걁뒶맟붎걂' instead of '銀河鉄道９９９（完成版）'.
    auto tryCodepage = [&raw](UINT cp, DWORD flags) -> QString {
        const int len = MultiByteToWideChar(cp, flags, raw.constData(), raw.size(), nullptr, 0);
        if (len <= 0) return QString();
        std::wstring buf(len, L'\0');
        if (MultiByteToWideChar(cp, flags, raw.constData(), raw.size(), buf.data(), len) <= 0)
            return QString();
        return QString::fromWCharArray(buf.data(), len);
    };
    auto hasHangul = [](const QString& s) {
        for (const QChar& c : s) {
            if (c.unicode() >= 0xAC00 && c.unicode() <= 0xD7A3) return true;
        }
        return false;
    };

    // Korean, then Japanese, each required to decode cleanly AND to yield
    // characters of that language.
    {
        const QString s = tryCodepage(949, MB_ERR_INVALID_CHARS);
        if (hasHangul(s)) return s;
    }
    {
        const QString s = tryCodepage(932, MB_ERR_INVALID_CHARS);
        for (const QChar& c : s) {
            const ushort u = c.unicode();
            if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x4E00 && u <= 0x9FFF)) return s;
        }
    }

    // Korean again, this time tolerating a bad byte: CP949 is the bulk of this
    // library, so a lyric with one damaged pair should still read as Korean
    // rather than fall through to Latin-1.
    {
        const QString s = tryCodepage(949, 0);
        if (hasHangul(s)) return s;
    }

    return QString::fromLatin1(raw);
}

QStringList MidiPlayer::extractLyrics() const
{
    QStringList lyrics;
    QByteArray currentBytes;

    if (tracks.empty()) {
        return lyrics;
    }

    // Look for lyric events (Meta Event 0x05) in all tracks
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            // The meta TYPE lives in event.data1 - parseMidiFile() reads it into
            // that field and puts only the payload in metaData (see the 0xFF
            // branch there). Reading metaData[0] as the type meant this matched
            // whatever the first text byte happened to be and then chopped that
            // byte off, so real lyric events were skipped and the few that got
            // through lost their first character (2026-07-31).
            if (event.isMetaEvent && !event.metaData.empty()) {
                const unsigned char metaType = event.data1;

                // 0x05 = Lyric, 0x01 = Text (some files carry lyrics there)
                if (metaType == 0x05 || metaType == 0x01) {
                    // A karaoke MIDI sends ONE SYLLABLE per lyric event and ends
                    // a line with a bare CR (or LF).
                    //
                    // Gather the RAW BYTES of a line and decode once at the end.
                    // Deciding the encoding per event cannot work: a lone
                    // syllable is two bytes, and two bytes are not enough
                    // evidence - '쩔' (c2 bf) failed the Hangul test and fell
                    // through to Latin-1 as '¿', which is where the stray
                    // characters mid-line came from (2026-07-31).
                    const char* p = reinterpret_cast<const char*>(event.metaData.data());
                    for (size_t i = 0; i < event.metaData.size(); ++i) {
                        if (p[i] == '\r' || p[i] == '\n') {
                            const QString line = decodeMetaText(currentBytes).trimmed();
                            if (!line.isEmpty()) lyrics.append(line);
                            currentBytes.clear();
                        } else if (p[i] == '^') {
                            // Sustain mark from the karaoke authoring tool, like
                            // the '-' in NOB/GYB lyrics. Not sung, not shown.
                            continue;
                        } else {
                            currentBytes.append(p[i]);
                        }
                    }
                }
            }
        }
    }

    // The last line usually has no trailing CR.
    {
        const QString line = decodeMetaText(currentBytes).trimmed();
        if (!line.isEmpty()) lyrics.append(line);
    }

    qDebug() << "[MidiPlayer] Extracted" << lyrics.size() << "lyric lines from MIDI file";
    return lyrics;
}

QList<MidiPlayer::MarkerEvent> MidiPlayer::extractMarkerTimings(int channel) const
{
    QList<MarkerEvent> markers;

    if (tracks.empty()) {
        return markers;
    }

    const int MARKER_CHANNEL = channel - 1; // Convert 1-based to 0-based index

    // 모든 트랙에서 지정된 채널의 Note On 이벤트 수집
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (!event.isMetaEvent && !event.isSysExEvent) {
                unsigned char status = event.status;
                int eventChannel = status & 0x0F;
                unsigned char messageType = status & 0xF0;

                // 지정된 채널의 Note On 이벤트만 추출
                if (eventChannel == MARKER_CHANNEL && messageType == 0x90 && event.data2 > 0) {
                    MarkerEvent marker;
                    marker.tick = (unsigned long)event.absoluteTimeUs; // 틱 정보 저장됨
                    marker.noteNumber = event.data1;
                    marker.velocity = event.data2;

                    // 틱을 밀리초로 변환
                    unsigned long long timeUs = ticksToMicroseconds(marker.tick);
                    marker.timeMs = (unsigned long)(timeUs / 1000);

                    markers.append(marker);
                }
            }
        }
    }

    // 타이밍 순서대로 정렬
    std::sort(markers.begin(), markers.end(), [](const MarkerEvent& a, const MarkerEvent& b) {
        return a.tick < b.tick;
    });

    // 준비용(인트로) 마커는 자동으로 제외
    if (markers.size() > 1 && ticksPerQuarter > 0) {
        unsigned long introGap = markers[1].tick - markers[0].tick;
        unsigned long skipThreshold = ticksPerQuarter * 4; // 1마디(4박) 이상 텀
        unsigned long extendedThreshold = ticksPerQuarter * 5;
        bool softerLeadIn = markers[0].velocity < markers[1].velocity;

        if (introGap >= skipThreshold && (softerLeadIn || introGap >= extendedThreshold)) {
            qDebug() << "[MidiPlayer] Removing intro marker on channel" << channel
                     << "(gap" << introGap << "ticks, threshold" << skipThreshold << ")";
            markers.removeFirst();
        }
    }

    qDebug() << "[MidiPlayer] Extracted" << markers.size() << "marker events from channel" << channel;
    if (!markers.isEmpty()) {
        qDebug() << "  First marker at tick:" << markers.first().tick << "ms:" << markers.first().timeMs;
        if (markers.size() > 1) {
            qDebug() << "  Second marker at tick:" << markers[1].tick << "ms:" << markers[1].timeMs;
        }
    }

    return markers;
}

void MidiPlayer::processEvents()
{
    QMutexLocker locker(&stateMutex);
    
    if (!playing || tracks.empty()) return;

    // Calculate elapsed real time since playback started
    qint64 currentRealTime = m_elapsedTimer.elapsed();
    qint64 elapsedMs = currentRealTime - playbackStartTime;
    qint64 logicalElapsedMs = elapsedMs * (m_userTempoScale / 100.0);

    // Convert elapsed time to MIDI ticks considering tempo changes
    currentTick = calculateCurrentTick(logicalElapsedMs);

    // Keep the member tempo in sync with the tempo map so getCurrentBpm()
    // reports the real file tempo (it previously stayed at the constructor
    // default 500000, so the UI showed 120 BPM for every MIDI file).
    // tempoMap is sorted by tick; take the last change at or before currentTick.
    for (const auto& tc : tempoMap) {
        if (tc.tick > currentTick) break;
        currentTempo = tc.tempo;
    }

    bool anyTrackActive = false;

    // Process events in each track
    for (auto& track : tracks) {
        while (track.currentEventIndex < track.events.size()) {
            const MidiEvent& event = track.events[track.currentEventIndex];
            unsigned long eventTick = (unsigned long)event.absoluteTimeUs; // Using as tick storage

            // Check if it's time to play this event
            if (eventTick <= currentTick) {
                if (event.isSysExEvent) {
                    sendSysExMessage(event.sysExData);
                } else if (!event.isMetaEvent) {
                    unsigned char status = event.status;
                    unsigned char data1 = event.data1;
                    unsigned char data2 = event.data2;

                    // Update channel state for seeking
                    updateChannelState(status, data1, data2);

                    // Emit signals for channel monitor
                    int channel = status & 0x0F;
                    unsigned char messageType = status & 0xF0;

                    if (messageType == 0x90 && data2 > 0) {
                        // Note On
                        emit noteOn(channel, data1, data2);
                    } else if (messageType == 0x80 || (messageType == 0x90 && data2 == 0)) {
                        // Note Off
                        emit noteOff(channel, data1);
                    } else if (messageType == 0xB0) {
                        // Control Change
                        emit controllerChange(channel, data1, data2);
                    } else if (messageType == 0xC0) {
                        // Program Change
                        emit programChange(channel, data1);
                    }

                    if ((status & 0xF0) == 0xB0 && data1 == 0x07) {
                        int channel = status & 0x0F;
                        // Store original volume from file
                        originalChannelVolumes[channel] = data2;
                        // Apply master volume: (original * master) / 127
                        // ONLY scale if using external MIDI device. Internal synth (JJoMeSynth)
                        // has its own master volume control handled by JJoMeSynth::setVolume().
                        if (!m_useInternalSynth) {
                            int adjustedVolume = (data2 * currentVolume) / 127;
                            data2 = static_cast<unsigned char>(qBound(0, adjustedVolume, 127));
                        }
                    }

                    // 채널 11 (index 10) 음소거 - 마커 채널은 재생하지 않음
                    if (currentChannelState[channel].muted) {
                        // 마커 채널은 소리 내지 않음 (채널 11)
                        qDebug() << "[MidiPlayer] Muted channel" << (channel + 1) << "event blocked";
                    } else {
                        sendMidiMessage(status, data1, data2);
                    }
                }

                track.currentEventIndex++;
            } else {
                anyTrackActive = true;
                break;
            }
        }

        if (track.currentEventIndex < track.events.size()) {
            anyTrackActive = true;
        }
    }

    // Process Events block end
    // Update local variables if needed before emitting length-taking signals
    
    // Unlock before emitting signals to prevent deadlock if connected slots
    locker.unlock();

    emit positionChanged((unsigned long)logicalElapsedMs);

    locker.relock();
    if (!anyTrackActive) {
        playing = false;
        paused = false;
        // Don't stop thread itself, just turn off play state

        // Send all notes off
        for (int channel = 0; channel < 16; channel++) {
            sendMidiMessage(0xB0 | channel, 0x7B, 0); // All notes off
            sendMidiMessage(0xB0 | channel, 0x78, 0); // All sound off
        }
        resetPlayback();
        locker.unlock();
        emit finished();
    }
}

bool MidiPlayer::parseMidiFile(const QString &filename)
{
    // MinGW의 std::ifstream은 wchar_t 경로를 지원하지 않아
    // A:\, B:\ 같은 플로피 드라이브 경로나 한글 경로에서 파일 열기가 실패할 수 있음.
    // Qt의 QFile이 내부적으로 CreateFileW()를 사용하므로 유니코드 경로를 올바르게 처리.
    QFile qfile(filename);
    if (!qfile.open(QIODevice::ReadOnly)) return false;
    QByteArray fileData = qfile.readAll();
    qfile.close();

    std::istringstream fileStream(std::string(fileData.constData(), fileData.size()));
    std::istream &file = fileStream;

    if (!file.good()) return false;


    tracks.clear();
    tempoMap.clear();

    // Read header
    char header[4];
    file.read(header, 4);
    if (strncmp(header, "MThd", 4) != 0) return false;

    // Header length
    unsigned long headerLength = 0;
    file.read(reinterpret_cast<char*>(&headerLength), 4);
    headerLength = _byteswap_ulong(headerLength);

    // Format type
    unsigned short format = 0;
    file.read(reinterpret_cast<char*>(&format), 2);
    format = _byteswap_ushort(format);

    // Number of tracks
    unsigned short numTracks = 0;
    file.read(reinterpret_cast<char*>(&numTracks), 2);
    numTracks = _byteswap_ushort(numTracks);

    // Time division
    unsigned short timeDivision = 0;
    file.read(reinterpret_cast<char*>(&timeDivision), 2);
    timeDivision = _byteswap_ushort(timeDivision);

    if (timeDivision & 0x8000) {
        return false;
    } else {
        ticksPerQuarter = timeDivision;
    }

    // Initialize tempo map with default tempo
    TempoChange initialTempo;
    initialTempo.tick = 0;
    initialTempo.tempo = 500000; // 120 BPM
    tempoMap.push_back(initialTempo);

    // Read tracks and collect tempo changes
    tracks.resize(numTracks);
    unsigned long maxTick = 0;

    for (int trackNum = 0; trackNum < numTracks; trackNum++) {
        char trackHeader[4];
        file.read(trackHeader, 4);
        if (strncmp(trackHeader, "MTrk", 4) != 0) continue;

        unsigned long trackLength = 0;
        file.read(reinterpret_cast<char*>(&trackLength), 4);
        trackLength = _byteswap_ulong(trackLength);

        std::streampos trackEnd = file.tellg() + std::streampos(trackLength);

        // Guard against a truncated file: a bad or interrupted copy leaves a
        // track header claiming more bytes than the file actually holds (three
        // of the FF6 set are exactly this - rounded to a 4 KB boundary). If
        // trackEnd points past EOF, the read loop below hits EOF, the stream
        // fails, and file.tellg() then returns -1 - which is always < trackEnd,
        // so the loop never ends and pushes garbage events until the process
        // runs out of memory and is killed. Clamp trackEnd to the real end so
        // the loop can terminate (2026-07-30).
        const std::streamoff fileSize = static_cast<std::streamoff>(fileData.size());
        if (static_cast<std::streamoff>(trackEnd) > fileSize)
            trackEnd = std::streampos(fileSize);

        MidiTrack& track = tracks[trackNum];
        track.currentEventIndex = 0;
        unsigned char runningStatus = 0;
        unsigned long absoluteTick = 0;

        // file.good() as well as the position check: once any read below hits
        // EOF the stream fails and tellg() returns -1, so the position test
        // alone would spin forever.
        while (file.good() && file.tellg() >= std::streampos(0) && file.tellg() < trackEnd) {
            unsigned long deltaTime = readVariableLength(file);
            absoluteTick += deltaTime;

            MidiEvent event;
            event.deltaTime = deltaTime;
            event.absoluteTimeUs = absoluteTick; // Store tick directly, no time conversion
            event.isMetaEvent = false;
            event.isSysExEvent = false;

            unsigned char status;
            file.read(reinterpret_cast<char*>(&status), 1);
            // Truncated mid-event: stop before a garbage length drives a huge
            // metaData/sysExData.resize() (readVariableLength on a failed stream
            // can return up to 28 bits). The while-condition catches the next
            // pass, but the resize happens before then, so bail out here.
            if (!file) break;
            if (status < 0x80) {
                status = runningStatus;
                file.seekg(-1, std::ios::cur);
            } else {
                runningStatus = status;
            }
            event.status = status;

            if (status == 0xFF) {
                event.isMetaEvent = true;
                file.read(reinterpret_cast<char*>(&event.data1), 1);
                unsigned long length = readVariableLength(file);
                event.metaData.resize(length);
                file.read(reinterpret_cast<char*>(event.metaData.data()), length);

                // Collect tempo changes
                if (event.data1 == 0x51 && length >= 3) {
                    unsigned long newTempo = (event.metaData[0] << 16) | (event.metaData[1] << 8) | event.metaData[2];
                    TempoChange tc;
                    tc.tick = absoluteTick;
                    tc.tempo = newTempo;
                    tempoMap.push_back(tc);
                }

                if (event.data1 == 0x2F) {
                    track.events.push_back(event);
                    break;
                }
            } else if (status == 0xF0 || status == 0xF7) {
                event.isSysExEvent = true;
                unsigned long length = readVariableLength(file);
                event.sysExData.resize(length);
                file.read(reinterpret_cast<char*>(event.sysExData.data()), length);
            } else {
                file.read(reinterpret_cast<char*>(&event.data1), 1);
                if ((status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0) {
                    file.read(reinterpret_cast<char*>(&event.data2), 1);
                } else {
                    event.data2 = 0;
                }
            }
            track.events.push_back(event);
        }
        maxTick = std::max(maxTick, absoluteTick);
    }

    // Sort tempo map. MUST be a stable sort: the default 120 BPM entry is pushed
    // first at tick 0, and files that define their own tempo at tick 0 rely on
    // "last entry at the same tick wins" in calculateCurrentTick()/duration calc.
    // std::sort is unstable and (with >16 entries, e.g. 999_GS2.MID's 18 tempo
    // events) reordered the two tick-0 entries so the 120 BPM default overrode
    // the file's real tempo -- the whole song played 25% slow (3:26 -> 4:13).
    std::stable_sort(tempoMap.begin(), tempoMap.end(), [](const TempoChange& a, const TempoChange& b) {
        return a.tick < b.tick;
    });

    // Seed the reported tempo with the file's tempo at tick 0 (last tick-0 entry
    // wins) so getCurrentBpm() is correct immediately after load, before playback.
    for (const auto& tc : tempoMap) {
        if (tc.tick > 0) break;
        currentTempo = tc.tempo;
    }

    // Calculate total duration using new method
    if (tempoMap.empty()) {
        totalDuration = (unsigned long)ticksToMilliseconds(maxTick, 500000);
    } else {
        double totalDurationMs = 0.0;
        unsigned long currentTick = 0;
        unsigned long currentTempo = 500000;

        for (const auto& tempoChange : tempoMap) {
            if (tempoChange.tick > currentTick) {
                totalDurationMs += ticksToMilliseconds(tempoChange.tick - currentTick, currentTempo);
            }
            currentTick = tempoChange.tick;
            currentTempo = tempoChange.tempo;
        }

        if (maxTick > currentTick) {
            totalDurationMs += ticksToMilliseconds(maxTick - currentTick, currentTempo);
        }

        totalDuration = (unsigned long)totalDurationMs;
    }

    // QFile은 이미 닫혀있으며, std::istream에는 close()가 없으므로 별도 처리 불필요
    return true;
}

unsigned long MidiPlayer::readVariableLength(std::istream &file)
{
    unsigned long value = 0;
    unsigned char byte;

    for (int i = 0; i < 4; i++) {
        file.read(reinterpret_cast<char*>(&byte), 1);
        value = (value << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) break;
    }

    return value;
}

void MidiPlayer::resetPlayback()
{
    for (auto& track : tracks) {
        track.currentEventIndex = 0;
        track.currentTime = 0; // Keep for compatibility, though not used in new timing system
    }
}

void MidiPlayer::sendMidiMessage(unsigned char status, unsigned char data1, unsigned char data2)
{
    unsigned char messageType = status & 0xF0;
    int channel = status & 0x0F;
    if (channel != 9 && (messageType == 0x90 || messageType == 0x80)) {
        if (messageType == 0x90 && data2 > 0) {
            // Note On: Apply and track the transposed pitch
            int transposed = data1 + m_userKeyTranspose;
            unsigned char finalNote = static_cast<unsigned char>(qBound(0, transposed, 127));
            m_transposedNotes[channel][data1] = finalNote;
            data1 = finalNote;
        } else {
            // Note Off (0x80, or 0x90 with velocity 0)
            int origNote = data1;
            if (m_transposedNotes[channel][origNote] != -1) {
                // If we have a tracked transposed pitch for this note, turn off that exact pitch
                data1 = static_cast<unsigned char>(m_transposedNotes[channel][origNote]);
                m_transposedNotes[channel][origNote] = -1; // Reset
            } else {
                int transposed = data1 + m_userKeyTranspose;
                data1 = static_cast<unsigned char>(qBound(0, transposed, 127));
            }
        }
    }

    if (m_useInternalSynth) {
        if (!connected) return;
        
        switch (messageType) {
            case 0x90:
                if (data2 > 0) {
                    // .okm/.okw masters a bit hot through the SoundFont; trim the
                    // note velocity slightly so it sits level with other formats.
                    float vel = data2 / 127.0f;
                    if (m_isOkmFile) vel *= 0.78f;
                    JJoMeSynth::instance().noteOn(channel, data1, vel);
                } else {
                    JJoMeSynth::instance().noteOff(channel, data1);
                }
                break;
            case 0x80:
                JJoMeSynth::instance().noteOff(channel, data1);
                break;
            case 0xB0:
                JJoMeSynth::instance().controlChange(channel, data1, data2);
                break;
            case 0xC0:
                JJoMeSynth::instance().programChange(channel, data1);
                break;
            case 0xE0:
                JJoMeSynth::instance().pitchBend(channel, data1 | (data2 << 7));
                break;
        }
        return;
    }

    // Nuked-SC55: raw bytes down the named pipe instead of a WinMM handle.
    if (m_bUseSc55 && m_pSc55) {
        if (connected)
            m_pSc55->SendShort(status, data1, data2);
        return;
    }

    // MT-32: straight into the library. mt32emu's queueing playMsg needs no
    // synchronisation with the rendering thread, so this can be called from
    // here without a handshake.
    if (m_bUseMt32 && m_pMt32) {
        if (connected)
            m_pMt32->SendShort(status, data1, data2);
        return;
    }

    if (!connected || !hMidiOut) return;

    DWORD message = status | (data1 << 8) | (data2 << 16);
    sendMidiMessage(message);
}

void MidiPlayer::sendMidiMessage(DWORD message)
{
    if (!connected) return;

    midiOutShortMsg(hMidiOut, message);
}

void MidiPlayer::sendSysExMessage(const std::vector<unsigned char> &data)
{
    if (!connected || data.empty()) return;

    // Build the complete framed message FIRST, before any device branch.
    //
    // Callers hand over the payload without framing - a song's SysEx event is
    // stored from the manufacturer ID onwards (`41 10 42 12 ...`), the 0xF0
    // having been consumed as the event's status byte. The Nuked-SC55 branch
    // used to send that verbatim, so the emulator saw 0x41 as a DATA byte under
    // whatever running status was current: no SysEx ever took effect (no GS
    // reset, no display banner) and the stray bytes came out as notes at the
    // wrong pitch and as clatter at the start of a song (2026-07-29).
    std::vector<unsigned char> sysExMessage;
    sysExMessage.reserve(data.size() + 2);
    sysExMessage.push_back(0xF0); // SysEx start

    // Add the data (excluding any existing F0/F7 bytes)
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] != 0xF0 && data[i] != 0xF7) {
            sysExMessage.push_back(data[i]);
        }
    }

    sysExMessage.push_back(0xF7); // SysEx end

    // Nuked-SC55: the emulated UART is a byte stream, so SysEx goes down the
    // same pipe as everything else - no MIDIHDR, no completion wait.
    if (m_bUseSc55 && m_pSc55) {
        m_pSc55->SendBytes(sysExMessage);
        return;
    }

    // MT-32: SysEx is how these files do most of their work - custom timbres,
    // part assignments, the display banner - so this path matters more here
    // than anywhere else. The bytes are already framed F0..F7, which is what
    // playSysex expects.
    if (m_bUseMt32 && m_pMt32) {
        m_pMt32->SendSysEx(sysExMessage);
        return;
    }

    // 내부 신스 모드에서는 hMidiOut이 nullptr이므로 SysEx를 보낼 수 없음
    if (m_useInternalSynth || !hMidiOut) return;

    // Prepare MIDIHDR structure for SysEx
    MIDIHDR midiHeader;
    memset(&midiHeader, 0, sizeof(MIDIHDR));

    // Set up the header
    midiHeader.lpData = reinterpret_cast<LPSTR>(sysExMessage.data());
    midiHeader.dwBufferLength = sysExMessage.size();
    midiHeader.dwFlags = 0;

    // Prepare and send the message
    MMRESULT result = midiOutPrepareHeader(hMidiOut, &midiHeader, sizeof(MIDIHDR));
    if (result == MMSYSERR_NOERROR) {
        ResetEvent(m_sysExEvent);
        result = midiOutLongMsg(hMidiOut, &midiHeader, sizeof(MIDIHDR));
        if (result == MMSYSERR_NOERROR) {
            // Wait for the message to be sent using Event
            WaitForSingleObject(m_sysExEvent, 200); // 200ms timeout
        }
        // NEVER unprepare a buffer the driver is still transmitting - that
        // truncates the SysEx mid-stream (the receiver sees a corrupted
        // message and drops it). Large OPL-tunnel SysEx over the 31250-baud
        // link can legitimately take >200ms end-to-end, so if the wait timed
        // out keep retrying while the driver says MIDIERR_STILLPLAYING.
        for (int tries = 0; tries < 40; ++tries) { // <= ~2s
            result = midiOutUnprepareHeader(hMidiOut, &midiHeader, sizeof(MIDIHDR));
            if (result != MIDIERR_STILLPLAYING)
                break;
            Sleep(50);
        }
    }
}

void MidiPlayer::updateVolumeToDevice()
{
    QMutexLocker locker(&stateMutex);
    if (!connected || (!hMidiOut && !m_useInternalSynth && !m_bUseSc55 && !m_bUseMt32)) return;

    // The MT-32 has a front-panel MASTER VOLUME and shows it on its display, so
    // that is what the slider drives. Scaling the sixteen CC#7s the way the
    // loop below does would fight whatever the song sets AND leave the panel
    // reading a number the user never chose (reported 2026-08-21: the display
    // said 98 while the slider said something else).
    if (m_bUseMt32 && m_pMt32) {
        const int vol = currentVolume;
        locker.unlock();
        m_pMt32->SetMasterVolume(vol);
        return;
    }

    // While the OPL register tunnel is running, the MIDI port IS the tunnel's
    // link (jmp -> Pico -> mt32-pi, one 31250 baud wire). Blasting 16 CC#7
    // messages down it buys nothing - the receiving COplTunnelSynth's
    // HandleMIDIShortMessage() is an empty function, so every one of them is
    // discarded - while costing 48 bytes = ~15 ms of wire time that the OPL
    // batches then have to wait behind. That delay is enough to push the
    // receiver's scheduling anchor past its cushion, which is why the sound
    // audibly falters exactly while the volume slider is being moved
    // (2026-07-27). Local output is muted during tunnelling anyway, so nothing
    // is lost by staying quiet here; ordinary MIDI playback is untouched.
    //
    // Two exemptions, because the original condition was just "not the internal
    // synth" and that is far wider than the problem it was written for. The
    // tunnel setting persists between runs, so anything it catches by accident
    // stays broken silently, with no connection to whatever the user last did.
    //
    //   Nuked-SC55 travels down a named pipe, not the MIDI wire, so nothing it
    //   receives can crowd the tunnel.
    //
    //   A MIDI file playing through this class means the OPL players are
    //   stopped, so the tunnel is sending nothing and the wire is ours. Only
    //   when an OPL song is the one tunnelling does the traffic actually
    //   compete - and that is exactly when `playing` is false here.
    //
    // Note that during a genuine OPL tunnel the slider cannot control the
    // jukebox at all: its COplTunnelSynth discards channel messages, so volume
    // there belongs to the jukebox's own control. Nothing is lost by staying
    // quiet in that case.
    if (!m_useInternalSynth && !m_bUseSc55 && !playing
        && OplTunnelSender::instance().isEnabled())
        return;

    // Send volume update to all channels based on their original values
    for (int channel = 0; channel < 16; channel++) {
        int adjustedVolume = originalChannelVolumes[channel];
        // Only scale if using external MIDI device. Internal synth (JJoMeSynth)
        // has its own master volume control handled by JJoMeSynth::setVolume().
        if (!m_useInternalSynth) {
            adjustedVolume = (adjustedVolume * currentVolume) / 127;
        }
        locker.unlock();
        sendMidiMessage(0xB0 | channel, 0x07, qBound(0, adjustedVolume, 127));
        locker.relock();
    }
}

void MidiPlayer::performSeek()
{
    QMutexLocker locker(&stateMutex);
    if (!connected || tracks.empty()) return;

    // Stop all notes and sounds immediately
    for (int channel = 0; channel < 16; channel++) {
        sendMidiMessage(0xB0 | channel, 0x7B, 0); // All notes off
        sendMidiMessage(0xB0 | channel, 0x78, 0); // All sound off
    }

    // Convert seek position (ms) to target tick using new method
    unsigned long targetTick = millisecondsToTicks((double)pendingSeekPosition, 500000);

    // Handle tempo changes to get accurate target tick
    if (!tempoMap.empty()) {
        unsigned long currentTick = 0;
        double remainingMs = (double)pendingSeekPosition;
        unsigned long currentTempo = 500000;

        for (const auto& tempoChange : tempoMap) {
            double timeToTempoChange = ticksToMilliseconds(tempoChange.tick - currentTick, currentTempo);

            if (remainingMs <= timeToTempoChange) {
                targetTick = currentTick + millisecondsToTicks(remainingMs, currentTempo);
                break;
            } else {
                currentTick = tempoChange.tick;
                remainingMs -= timeToTempoChange;
                currentTempo = tempoChange.tempo;
            }
        }

        if (remainingMs > 0) {
            targetTick = currentTick + millisecondsToTicks(remainingMs, currentTempo);
        }
    }

    // Reset channel state to defaults
    initializeChannelState();

    // Position tracks to target tick
    for (auto& track : tracks) {
        track.currentEventIndex = 0;
        track.currentTime = 0;

        while (track.currentEventIndex < track.events.size()) {
            const MidiEvent& event = track.events[track.currentEventIndex];
            unsigned long eventTick = (unsigned long)event.absoluteTimeUs; // Using as tick storage

            if (eventTick <= targetTick) {
                track.currentEventIndex++;
            } else {
                break;
            }
        }
    }

    // Update timing for real-time playback
    if (playing) {
        playbackStartTime = m_elapsedTimer.elapsed() - (pendingSeekPosition / (m_userTempoScale / 100.0));
    } else if (paused) {
        pausedTime = pendingSeekPosition;
    }

    // Clear seek pending flag
    setProperty("seekPending", false);
}

void MidiPlayer::performSeekImmediate(unsigned long position)
{
    QMutexLocker locker(&stateMutex);
    if (!connected || tracks.empty()) return;

    // Stop all notes and sounds immediately
    for (int channel = 0; channel < 16; channel++) {
        sendMidiMessage(0xB0 | channel, 0x7B, 0); // All notes off
        sendMidiMessage(0xB0 | channel, 0x78, 0); // All sound off
    }

    // Convert seek position (ms) to target tick using new method
    unsigned long targetTick = millisecondsToTicks((double)position, 500000);

    // Handle tempo changes to get accurate target tick
    if (!tempoMap.empty()) {
        unsigned long currentTick = 0;
        double remainingMs = (double)position;
        unsigned long currentTempo = 500000;

        for (const auto& tempoChange : tempoMap) {
            double timeToTempoChange = ticksToMilliseconds(tempoChange.tick - currentTick, currentTempo);

            if (remainingMs <= timeToTempoChange) {
                targetTick = currentTick + millisecondsToTicks(remainingMs, currentTempo);
                break;
            } else {
                currentTick = tempoChange.tick;
                remainingMs -= timeToTempoChange;
                currentTempo = tempoChange.tempo;
            }
        }

        if (remainingMs > 0) {
            targetTick = currentTick + millisecondsToTicks(remainingMs, currentTempo);
        }
    }

    // Reset channel state to defaults
    initializeChannelState();

    // Position tracks to target tick and build correct MIDI state
    for (auto& track : tracks) {
        track.currentEventIndex = 0;
        track.currentTime = 0;

        while (track.currentEventIndex < track.events.size()) {
            const MidiEvent& event = track.events[track.currentEventIndex];
            unsigned long eventTick = (unsigned long)event.absoluteTimeUs; // Using as tick storage

            if (eventTick <= targetTick) {
                // Track state-changing events (but don't send yet)
                if (!event.isMetaEvent && !event.isSysExEvent) {
                    unsigned char status = event.status;

                    // Only process state-changing messages (CC, PC, PB)
                    if ((status & 0xF0) == 0xB0 || (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xE0) {
                        updateChannelState(status, event.data1, event.data2);

                        // Store original volume for master volume calculation
                        if ((status & 0xF0) == 0xB0 && event.data1 == 0x07) {
                            int channel = status & 0x0F;
                            originalChannelVolumes[channel] = event.data2;
                        }
                    }
                }

                track.currentEventIndex++;
            } else {
                break;
            }
        }
    }

    // Send the final state to restore correct instruments and settings
    sendCurrentChannelState();

    // Special case: if seeking to beginning, reset to start
    if (position == 0) {
        for (auto& track : tracks) {
            track.currentEventIndex = 0;
            track.currentTime = 0;
        }
    }

    // Update timing for real-time playback
    if (playing) {
        playbackStartTime = m_elapsedTimer.elapsed() - (position / (m_userTempoScale / 100.0));
    } else if (paused) {
        pausedTime = position;
    }
}

void MidiPlayer::initializeChannelState()
{
    for (int i = 0; i < 16; i++) {
        currentChannelState[i].volume = 100;
        currentChannelState[i].expression = 127;
        currentChannelState[i].program = 0;
        currentChannelState[i].pitchBend = 8192; // Center position
        currentChannelState[i].modWheel = 0;
        currentChannelState[i].sustain = 0;
        // NOB 파일일 때만 채널 11 (index 10)을 음소거
        currentChannelState[i].muted = (isNobFile && i == 10);
    }

    for (int ch = 0; ch < 16; ch++) {
        for (int note = 0; note < 128; note++) {
            m_transposedNotes[ch][note] = -1;
        }
    }

    if (isNobFile) {
        qDebug() << "[MidiPlayer] Channel 11 muted for NOB file";
    }
}

void MidiPlayer::updateChannelState(unsigned char status, unsigned char data1, unsigned char data2)
{
    if ((status & 0xF0) == 0xB0) {
        // Control Change
        int channel = status & 0x0F;
        switch (data1) {
            case 1:  currentChannelState[channel].modWheel = data2; break;
            case 7:  currentChannelState[channel].volume = data2; break;
            case 11: currentChannelState[channel].expression = data2; break;
            case 64: currentChannelState[channel].sustain = data2; break;
        }
    } else if ((status & 0xF0) == 0xC0) {
        // Program Change
        int channel = status & 0x0F;
        currentChannelState[channel].program = data1;
    } else if ((status & 0xF0) == 0xE0) {
        // Pitch Bend
        int channel = status & 0x0F;
        currentChannelState[channel].pitchBend = data1 | (data2 << 7);
    }
}

void MidiPlayer::sendCurrentChannelState()
{
    if (!connected || (!hMidiOut && !m_useInternalSynth)) return;

    for (int channel = 0; channel < 16; channel++) {
        // Send control changes
        sendMidiMessage(0xB0 | channel, 1, currentChannelState[channel].modWheel);

        // Apply master volume to channel volume ONLY if not using internal synth
        int adjustedVolume = currentChannelState[channel].volume;
        if (!m_useInternalSynth) {
            adjustedVolume = (adjustedVolume * currentVolume) / 127;
        }
        sendMidiMessage(0xB0 | channel, 7, qBound(0, adjustedVolume, 127));

        sendMidiMessage(0xB0 | channel, 11, currentChannelState[channel].expression);
        sendMidiMessage(0xB0 | channel, 64, currentChannelState[channel].sustain);

        // Send program change
        sendMidiMessage(0xC0 | channel, currentChannelState[channel].program, 0);

        // Send pitch bend
        int pitchBend = currentChannelState[channel].pitchBend;
        sendMidiMessage(0xE0 | channel, pitchBend & 0x7F, (pitchBend >> 7) & 0x7F);
    }
}

unsigned long long MidiPlayer::ticksToMicroseconds(unsigned long ticks) const
{
    if (tempoMap.empty() || ticksPerQuarter == 0) {
        return (unsigned long long)ticks * 500000 / ticksPerQuarter; // Use actual ticksPerQuarter
    }

    unsigned long long totalMicroseconds = 0;
    unsigned long currentTick = 0;
    unsigned long currentTempo = 500000; // Default tempo (120 BPM)

    for (const auto& tempoChange : tempoMap) {
        if (tempoChange.tick > ticks) {
            // Calculate remaining time with current tempo
            totalMicroseconds += (unsigned long long)(ticks - currentTick) * currentTempo / ticksPerQuarter;
            break;
        }

        // Add time from currentTick to this tempo change
        if (tempoChange.tick > currentTick) {
            totalMicroseconds += (unsigned long long)(tempoChange.tick - currentTick) * currentTempo / ticksPerQuarter;
        }

        // Update for next iteration
        currentTick = tempoChange.tick;
        currentTempo = tempoChange.tempo;
    }

    // Handle case where ticks is beyond all tempo changes
    if (ticks > currentTick) {
        totalMicroseconds += (unsigned long long)(ticks - currentTick) * currentTempo / ticksPerQuarter;
    }

    return totalMicroseconds;
}

// New real-time based functions
unsigned long MidiPlayer::calculateCurrentTick(unsigned long long elapsedMs) const
{
    if (tempoMap.empty()) {
        return millisecondsToTicks((double)elapsedMs, 500000);
    }

    unsigned long currentTick = 0;
    double remainingMs = (double)elapsedMs;
    unsigned long currentTempo = 500000;

    for (const auto& tempoChange : tempoMap) {
        double timeToTempoChange = ticksToMilliseconds(tempoChange.tick - currentTick, currentTempo);

        if (remainingMs <= timeToTempoChange) {
            currentTick += millisecondsToTicks(remainingMs, currentTempo);
            return currentTick;
        } else {
            currentTick = tempoChange.tick;
            remainingMs -= timeToTempoChange;
            currentTempo = tempoChange.tempo;
        }
    }

    currentTick += millisecondsToTicks(remainingMs, currentTempo);
    return currentTick;
}

double MidiPlayer::ticksToMilliseconds(unsigned long ticks, unsigned long tempo) const
{
    if (ticksPerQuarter == 0) return 0.0;
    return (double)ticks * tempo / (ticksPerQuarter * 1000.0);
}

unsigned long MidiPlayer::millisecondsToTicks(double ms, unsigned long tempo) const
{
    if (tempo == 0) return 0;
    return (unsigned long)(ms * ticksPerQuarter * 1000.0 / tempo);
}

void MidiPlayer::createGlobalEventList()
{
    allEvents.clear();

    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        for (size_t eventIndex = 0; eventIndex < tracks[trackIndex].events.size(); ++eventIndex) {
            const MidiEvent& event = tracks[trackIndex].events[eventIndex];
            if (!event.isMetaEvent || (event.status == 0xFF && event.data1 == 0x51)) {
                // Include non-meta events and tempo changes
                allEvents.push_back(std::make_pair((unsigned long)event.absoluteTimeUs,
                                                 (unsigned long)(trackIndex * 10000 + eventIndex)));
            }
        }
    }

    // Sort all events by tick
    std::sort(allEvents.begin(), allEvents.end(),
              [](const std::pair<unsigned long, unsigned long>& a,
                 const std::pair<unsigned long, unsigned long>& b) {
                  return a.first < b.first;
              });
}

void MidiPlayer::detectSoundMode()
{
    SoundModeReliability reliability = calculateSoundModeReliability();
    // Emit signals to update channel monitor
    emit soundModeDetected(reliability.detectedMode);
    emit soundModeReliabilityChanged(reliability);
}

SoundModeReliability MidiPlayer::calculateSoundModeReliability()
{
    // DISPLAY ONLY. The result reaches nothing but the channel monitor's mode
    // label and its instrument-name table; no playback path reads it.
    //
    // This used to add up scores from many weak rules, and the weak ones
    // outvoted the strong ones. Measured over a 1,278-file library: 415 files
    // (32%) were called MT-32 while exactly one contained an MT-32 reset, and
    // of the 94 files carrying an explicit reset message only 64 were
    // identified correctly. Three rules did the damage:
    //
    //   * programs 64-127 counted as "MT-32 characteristic" - that is half the
    //     GM sound map, so almost every file matched;
    //   * "does not use channel 1" awarded MT-32 25 points, enough on its own
    //     to beat GM's base score;
    //   * partial vendor matches added 60 points *per event*, so one file
    //     reached 6,780 MT-32 points and could no longer be outvoted.
    //
    // Worse, the single most decisive message - GM System On - was not checked
    // at all, so a file that declared itself GM scored nothing for it.
    //
    // Evidence is now ranked instead of summed, strongest first, and each rule
    // counts once. The same 94 files now come out 94/94.
    //
    // A file that declares nothing gets its own state rather than being counted
    // as GM - 1,087 of the 1,278 measured say nothing at all, and reporting
    // those as GM stated a fact the file does not contain. The monitor shows
    // the state as "GM (assumed)": the reading is still GM, because that is
    // what an undeclared MIDI means, but it is marked as an assumption and
    // greyed so the 40 files that really do declare GM stay distinguishable.
    // Attempts to infer the module from channel layout, drum notes or
    // controller use were all measured and rejected; see the note further down.
    SoundModeReliability reliability;
    reliability.detectedMode = -1;              // ChannelWidget::UNKNOWN_MODE
    reliability.confidenceScore = 0;
    reliability.detectionMethod = "No declaration in file";
    reliability.evidenceList.clear();
    reliability.hasStrongEvidence = false;
    reliability.alternativeMode = -1;

    // Roland and Yamaha both put a device ID in byte 1. Only 0x10 was accepted
    // before; the spec allows 0x00-0x1F.
    auto isDeviceId = [](unsigned char b) { return b <= 0x1F; };

    int  resetMode  = -1;      // explicit device reset (strongest)
    bool sawGmOn    = false;   // GM System On - real, but any GS/XG reset follows it
    QSet<int> vendorFamilies;  // manufacturer+model seen, without a reset
    QStringList sysExEvidence;

    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (!event.isSysExEvent) continue;
            const std::vector<unsigned char>& s = event.sysExData;   // no leading F0
            const size_t n = s.size();

            // --- Universal: GM System On / GM2 System On -------------------
            if (n >= 4 && s[0] == 0x7E && s[2] == 0x09 && (s[3] == 0x01 || s[3] == 0x03)) {
                if (!sawGmOn) {
                    sawGmOn = true;
                    sysExEvidence.append("GM System On");
                }
                continue;
            }

            // --- Roland ----------------------------------------------------
            if (n >= 3 && s[0] == 0x41 && isDeviceId(s[1])) {
                // GS Reset: 41 dev 42 12 40 00 7F 00 41 F7
                if (n >= 8 && s[2] == 0x42 && s[3] == 0x12 &&
                    s[4] == 0x40 && s[5] == 0x00 && s[6] == 0x7F && s[7] == 0x00) {
                    resetMode = 2;
                    if (!sysExEvidence.contains("GS Reset")) sysExEvidence.append("GS Reset");
                }
                // MT-32 reset: 41 dev 16 12 7F ...
                else if (n >= 5 && s[2] == 0x16 && s[3] == 0x12 && s[4] == 0x7F) {
                    resetMode = 1;
                    if (!sysExEvidence.contains("MT-32 Reset")) sysExEvidence.append("MT-32 Reset");
                }
                else if (s[2] == 0x42) vendorFamilies.insert(2);
                else if (s[2] == 0x16) vendorFamilies.insert(1);
                continue;
            }

            // --- Yamaha ----------------------------------------------------
            if (n >= 3 && s[0] == 0x43 && isDeviceId(s[1])) {
                // XG System On: 43 dev 4C 00 00 7E 00 F7
                if (n >= 6 && s[2] == 0x4C && s[3] == 0x00 && s[4] == 0x00 && s[5] == 0x7E) {
                    resetMode = 3;
                    if (!sysExEvidence.contains("XG System On")) sysExEvidence.append("XG System On");
                }
                else if (s[2] == 0x4C) vendorFamilies.insert(3);
            }
        }
    }

    // An MT-32 has nine parts: eight melodic plus rhythm. Notes on channels
    // 11-16, or simply more than nine channels in play, rule it out. Measured
    // over 71 files carrying MT-32 SysEx, not one used a tenth channel.
    // Used only to reject a guess, never to override a file's own reset.
    //
    // Nothing here ever *concludes* MT-32 - a file that says nothing about
    // itself is left as GM. Guessing was tried and rejected: absence of Bank
    // Select and reverb/chorus send separates the two ~87:1 on files that carry
    // SysEx (MT-32 6-8% vs GM family 89-99%), but among the silent files it
    // fires far too often - it moved 522 of them to MT-32, more than the 415
    // false positives this rewrite was meant to remove, so it was taken out
    // again. Channel count and drum-note choice are weaker still (~1.9:1).
    QSet<int> noteChannels;
    bool usesHighChannels = false;
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (event.isMetaEvent || event.isSysExEvent) continue;
            if ((event.status & 0xF0) == 0x90 && event.data2 > 0) {
                const int channel = event.status & 0x0F;
                noteChannels.insert(channel);
                if (channel >= 10) usesHighChannels = true;
            }
        }
    }
    const bool mt32Impossible = usesHighChannels || noteChannels.size() > 9;

    // Text is the weakest evidence and needs whole-word matching: "gs" as a
    // substring hits "strings", "mu" hits "drums" and "music".
    static const QRegularExpression reMt32("\\bmt[- ]?32\\b");
    static const QRegularExpression reGs("\\bgs\\b");
    static const QRegularExpression reXg("\\bxg\\b");
    int textMode = -1;
    QString textHit;
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (!event.isMetaEvent) continue;
            if (event.status != 0xFF) continue;
            if (event.data1 != 0x01 && event.data1 != 0x02 && event.data1 != 0x03) continue;

            const QString original = QString::fromLatin1(
                reinterpret_cast<const char*>(event.metaData.data()),
                static_cast<int>(event.metaData.size())).trimmed();
            const QString t = original.toLower();

            int hit = -1;
            if (t.contains(reMt32) || t.contains("roland mt")) hit = 1;
            else if (t.contains(reGs) && (t.contains("roland") || t.contains("sound canvas"))) hit = 2;
            else if (t.contains("general standard")) hit = 2;
            else if (t.contains(reXg) || t.contains("extended general")) hit = 3;

            if (hit != -1 && textMode == -1) {
                textMode = hit;
                textHit = original;
            }
        }
        if (textMode != -1) break;
    }

    // ---- Rank the evidence ---------------------------------------------
    if (resetMode != -1) {
        reliability.detectedMode    = resetMode;
        reliability.confidenceScore = 98;
        reliability.detectionMethod = "SysEx reset";
    } else if (sawGmOn) {
        reliability.detectedMode    = 0;
        reliability.confidenceScore = 90;
        reliability.detectionMethod = "SysEx reset";
    } else if (!vendorFamilies.isEmpty()) {
        int pick = -1;
        if (vendorFamilies.contains(2))      pick = 2;   // GS
        else if (vendorFamilies.contains(3)) pick = 3;   // XG
        else if (vendorFamilies.contains(1) && !mt32Impossible) pick = 1;
        if (pick != -1) {
            reliability.detectedMode    = pick;
            reliability.confidenceScore = 70;
            reliability.detectionMethod = "SysEx (vendor)";
            sysExEvidence.append(pick == 1 ? "Roland MT-32 messages"
                               : pick == 2 ? "Roland GS messages"
                                           : "Yamaha XG messages");
        }
    }

    if (reliability.detectedMode == -1 && textMode != -1) {
        if (!(textMode == 1 && mt32Impossible)) {
            reliability.detectedMode    = textMode;
            reliability.confidenceScore = 50;
            reliability.detectionMethod = "Text";
            reliability.evidenceList.append("Text: \"" + textHit + "\"");
        }
    }

    reliability.evidenceList = sysExEvidence + reliability.evidenceList;
    if (mt32Impossible && reliability.detectedMode != 1)
        reliability.evidenceList.append("Too many parts for an MT-32");
    if (reliability.evidenceList.isEmpty())
        reliability.evidenceList.append("No specific mode indicators found");

    reliability.hasStrongEvidence = (reliability.confidenceScore >= 80);

    // The monitor ranks these; give the winner its confidence and leave a
    // trace of anything else that was actually seen.
    QMap<int, int> modeScores;
    for (int m = 0; m <= 3; ++m) modeScores[m] = 0;
    if (reliability.detectedMode >= 0)   // Unknown is not one of the four
        modeScores[reliability.detectedMode] = reliability.confidenceScore;
    for (int fam : vendorFamilies)
        if (fam != reliability.detectedMode) modeScores[fam] = qMax(modeScores[fam], 25);
    if (textMode != -1 && textMode != reliability.detectedMode)
        modeScores[textMode] = qMax(modeScores[textMode], 20);
    reliability.allModeScores = modeScores;

    if (reliability.confidenceScore < 60) {
        int best = 0;
        for (auto it = modeScores.begin(); it != modeScores.end(); ++it) {
            if (it.key() != reliability.detectedMode && it.value() > best) {
                best = it.value();
                reliability.alternativeMode = it.key();
            }
        }
        if (best < 15) reliability.alternativeMode = -1;
    }

    reliability.confusionHint = getConfusionHint(reliability.alternativeMode,
                                                 reliability.confidenceScore);
    return reliability;
}

DetectionStrength MidiPlayer::getDetectionStrength(int confidenceScore)
{
    if (confidenceScore >= 80) return STRONG_DETECTION;
    if (confidenceScore >= 40) return MODERATE_DETECTION;
    return WEAK_DETECTION;
}

int MidiPlayer::getMostLikelyAlternative(int detectedMode, int confidenceScore)
{
    // Only show alternatives for low confidence detections
    if (confidenceScore >= 60) {
        return -1; // No alternative shown for moderate-high confidence
    }

    // This will be replaced by score-based ranking in calculateSoundModeReliability
    return -1;
}

QString MidiPlayer::getConfusionHint(int alternativeMode, int confidenceScore)
{
    if (alternativeMode == -1) {
        return ""; // No confusion hint
    }

    // Convert mode number to readable text
    QString altModeText;
    switch (alternativeMode) {
        case 0: altModeText = "GM"; break;
        case 1: altModeText = "MT-32"; break;
        case 2: altModeText = "GS"; break;
        case 3: altModeText = "XG"; break;
        default: return "";
    }

    return QString(" (or %1)").arg(altModeText);
}

// ---------------------------------------------------------
// PlaybackThread Implementation
// ---------------------------------------------------------

void PlaybackThread::run()
{
    m_running = true;
    
    while (m_running) {
        bool isPlaying = false;
        {
            QMutexLocker locker(&m_player->stateMutex);
            isPlaying = m_player->playing;
        }
        
        if (isPlaying) {
            // Process MIDI events - processEvents() handles its own locking
            m_player->processEvents();
            
            // Sleep 1ms for high-resolution timing without burning 100% CPU
            QThread::msleep(1);
        } else {
            // When not playing, sleep longer to save CPU
            QThread::msleep(10);
        }
    }
}
void MidiPlayer::setUseInternalSynth(bool useInternal, const QString& soundFontPath)
{
    QMutexLocker locker(&stateMutex);
    
    // If playing, we shouldn't change synth state directly here, 
    // it's better handled by disconnect/connectToDevice logic in MainWindow.
    m_useInternalSynth = useInternal;
    
    if (useInternal) {
        if (!JJoMeSynth::instance().isInitialized() || (!soundFontPath.isEmpty() && m_currentSoundFontPath != soundFontPath)) {
            if (!soundFontPath.isEmpty()) {
                JJoMeSynth::instance().initialize(soundFontPath);
                m_currentSoundFontPath = soundFontPath;
            }
        }
    }
}

void MidiPlayer::setUserKeyTranspose(int key)
{
    QMutexLocker locker(&stateMutex);
    int newKey = qBound(-6, key, 6);
    if (newKey != m_userKeyTranspose) {
        // Send Note Off for all currently active transposed notes to avoid stuck notes during shift
        for (int ch = 0; ch < 16; ch++) {
            if (ch == 9) continue; // Skip drum channel
            for (int note = 0; note < 128; note++) {
                if (m_transposedNotes[ch][note] != -1) {
                    unsigned char finalNote = m_transposedNotes[ch][note];
                    
                    // JJoMeSynth 또는 hMidiOut에 직접 전송하여 2중 Transpose 보정 회피
                    if (m_useInternalSynth) {
                        JJoMeSynth::instance().noteOff(ch, finalNote);
                    } else if (hMidiOut) {
                        DWORD msg = (0x80 | ch) | (finalNote << 8) | (0 << 16);
                        midiOutShortMsg(hMidiOut, msg);
                    }
                    
                    m_transposedNotes[ch][note] = -1; // Reset tracking
                }
            }
        }
        m_userKeyTranspose = newKey;
    }
}

int MidiPlayer::getUserKeyTranspose() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    return m_userKeyTranspose;
}

void MidiPlayer::setUserTempoScale(int scale)
{
    QMutexLocker locker(&stateMutex);
    int newScale = qBound(50, scale, 150);
    if (newScale == m_userTempoScale) return;

    // The logical song time is `elapsed * (scale/100)`. If we change the scale
    // with playbackStartTime fixed, the logical time (and thus currentTick)
    // jumps instantly — which re-seeks the song mid-note and breaks/sticks the
    // sound. Re-anchor playbackStartTime so the CURRENT logical position is
    // preserved and only the future playback rate changes.
    if (playing && !paused) {
        qint64 now = m_elapsedTimer.elapsed();
        double oldF = m_userTempoScale / 100.0;
        double newF = newScale / 100.0;
        playbackStartTime = now - (qint64)((double)(now - playbackStartTime) * oldF / newF);
    }
    // When paused, pausedTime already holds scale-independent logical ms, so the
    // resume path recomputes playbackStartTime correctly with the new scale.

    m_userTempoScale = newScale;
}

int MidiPlayer::getUserTempoScale() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    return m_userTempoScale;
}

int MidiPlayer::getCurrentBpm() const
{
    QMutexLocker locker(const_cast<QMutex*>(&stateMutex));
    if (currentTempo == 0) return 120;
    double baseBpm = 60000000.0 / currentTempo;
    return static_cast<int>(baseBpm * (m_userTempoScale / 100.0) + 0.5);
}
