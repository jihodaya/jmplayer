#include "midiplayer.h"
#include "nobfilehandler.h"
#include "okafilehandler.h"
#include "opltunnelsender.h" // to stay off the wire while the OPL tunnel owns it
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDir>
#include <QCoreApplication>
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

bool MidiPlayer::loadMidiFile(const QString &filename)
{
    // Stop playback before modifying tracks to prevent background thread crash
    stop();
    
    QString actualFilename = filename;
    QString tempMidiPath;

    // NOB 파일인 경우 MIDI 데이터 추출
    if (filename.toLower().endsWith(".nob")) {
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
    if (connected && (hMidiOut || m_useInternalSynth || m_bUseSc55)) {
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
    if (!connected || (!hMidiOut && !m_useInternalSynth && !m_bUseSc55)) return;

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
    if (!m_useInternalSynth && OplTunnelSender::instance().isEnabled())
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
    SoundModeReliability reliability;
    reliability.detectedMode = 0; // Default to GM
    reliability.confidenceScore = 25; // Slightly higher base score for GM
    reliability.detectionMethod = "Default (GM assumed)";
    reliability.evidenceList.clear();
    reliability.hasStrongEvidence = false;

    // Initialize all mode scores
    QMap<int, int> modeScores;
    modeScores[0] = 25; // GM base score
    modeScores[1] = 0;  // MT-32
    modeScores[2] = 0;  // GS
    modeScores[3] = 0;  // XG

    int sysExScore = 0;
    int textScore = 0;
    int evidenceStrength = 0; // Track strongest evidence found
    QStringList sysExEvidence;
    QStringList textEvidence;

    // Search through all tracks for sound mode indicators
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (event.isMetaEvent) {
                // Check for text meta events that might indicate sound mode
                if (event.status == 0xFF && (event.data1 == 0x01 || event.data1 == 0x02 || event.data1 == 0x03)) {
                    QString text = QString::fromLatin1((const char*)event.metaData.data(), event.metaData.size());
                    QString originalText = text;
                    text = text.toLower();

                    // Strong MT-32 text detection
                    if (text.contains("mt-32") || text.contains("mt32") || text.contains("roland mt")) {
                        modeScores[1] += 75; // MT-32 mode
                        if (evidenceStrength < 75) {
                            textScore = 75; // Strong text evidence
                            evidenceStrength = 75;
                            textEvidence.append("Strong text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    // Strong GS text detection - improved conditions
                    else if ((text.contains("gs") && (text.contains("roland") || text.contains("sound canvas"))) ||
                             text.contains("roland gs") || text.contains("general standard")) {
                        modeScores[2] += 75; // GS mode
                        if (evidenceStrength < 75) {
                            textScore = 75; // Strong text evidence
                            evidenceStrength = 75;
                            textEvidence.append("Strong text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    // Strong XG text detection - improved conditions
                    else if ((text.contains("xg") && (text.contains("yamaha") || text.contains("mu"))) ||
                             text.contains("yamaha xg") || text.contains("extended general")) {
                        modeScores[3] += 75; // XG mode
                        if (evidenceStrength < 75) {
                            textScore = 75; // Strong text evidence
                            evidenceStrength = 75;
                            textEvidence.append("Strong text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    // Moderate text indicators - standalone format names
                    else if (text.contains(" gs ") || text.contains("gs mode") || text.contains("gs midi")) {
                        modeScores[2] += 50; // GS mode
                        if (evidenceStrength < 50) {
                            textScore = 50; // Moderate text evidence
                            evidenceStrength = 50;
                            textEvidence.append("Moderate text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    else if (text.contains(" xg ") || text.contains("xg mode") || text.contains("xg midi")) {
                        modeScores[3] += 50; // XG mode
                        if (evidenceStrength < 50) {
                            textScore = 50; // Moderate text evidence
                            evidenceStrength = 50;
                            textEvidence.append("Moderate text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    // Weak text indicators - manufacturer names only
                    else if (text.contains("roland") && !text.contains("gs") && !text.contains("mt")) {
                        modeScores[2] += 30; // GS mode (Roland default)
                        if (evidenceStrength < 30) {
                            textScore = 30; // Weak text evidence
                            evidenceStrength = 30;
                            textEvidence.append("Weak text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    else if (text.contains("yamaha") && !text.contains("xg")) {
                        modeScores[3] += 30; // XG mode
                        if (evidenceStrength < 30) {
                            textScore = 30; // Weak text evidence
                            evidenceStrength = 30;
                            textEvidence.append("Weak text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    // Very weak indicators - common music software terms
                    else if (text.contains("sound canvas") || text.contains("sc-") || text.contains("sc ")) {
                        modeScores[2] += 20; // GS mode
                        if (evidenceStrength < 20) {
                            textScore = 20;
                            evidenceStrength = 20;
                            textEvidence.append("Very weak text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                    else if (text.contains("mu80") || text.contains("mu100") || text.contains("motif")) {
                        modeScores[3] += 20; // XG mode
                        if (evidenceStrength < 20) {
                            textScore = 20;
                            evidenceStrength = 20;
                            textEvidence.append("Very weak text: \"" + originalText.trimmed() + "\"");
                        }
                    }
                }
            } else if (event.isSysExEvent) {
                // Check SysEx messages for device-specific resets with improved pattern matching

                // MT-32 Reset: F0 41 10 16 12 7F 00 01 00 F7 (complete 10-byte pattern)
                if (event.sysExData.size() >= 8 &&
                    event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                    event.sysExData[2] == 0x16 && event.sysExData[3] == 0x12 &&
                    event.sysExData[4] == 0x7F && event.sysExData[5] == 0x00) {
                    modeScores[1] += 95; // MT-32 mode
                    if (evidenceStrength < 95) {
                        sysExScore = 95; // Very strong SysEx evidence (complete pattern)
                        evidenceStrength = 95;
                        sysExEvidence.append("MT-32 Master Reset SysEx (complete)");
                    }
                }
                // GS Reset: F0 41 10 42 12 40 00 7F 00 41 F7 (complete pattern)
                else if (event.sysExData.size() >= 9 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x42 && event.sysExData[3] == 0x12 &&
                         event.sysExData[4] == 0x40 && event.sysExData[5] == 0x00 &&
                         event.sysExData[6] == 0x7F && event.sysExData[7] == 0x00) {
                    modeScores[2] += 95; // GS mode
                    if (evidenceStrength < 95) {
                        sysExScore = 95; // Very strong SysEx evidence
                        evidenceStrength = 95;
                        sysExEvidence.append("GS Reset SysEx (complete)");
                    }
                }
                // XG Reset: F0 43 10 4C 00 00 7E 00 F7 (complete pattern)
                else if (event.sysExData.size() >= 7 &&
                         event.sysExData[0] == 0x43 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x4C && event.sysExData[3] == 0x00 &&
                         event.sysExData[4] == 0x00 && event.sysExData[5] == 0x7E &&
                         event.sysExData[6] == 0x00) {
                    modeScores[3] += 95; // XG mode
                    if (evidenceStrength < 95) {
                        sysExScore = 95; // Very strong SysEx evidence
                        evidenceStrength = 95;
                        sysExEvidence.append("XG System On SysEx (complete)");
                    }
                }
                // Partial Roland SysEx patterns (moderate evidence)
                else if (event.sysExData.size() >= 4 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x42) {
                    modeScores[2] += 60; // GS mode
                    if (evidenceStrength < 60) {
                        sysExScore = 60; // Moderate SysEx evidence
                        evidenceStrength = 60;
                        sysExEvidence.append("Roland GS SysEx (partial)");
                    }
                }
                else if (event.sysExData.size() >= 4 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x16) {
                    modeScores[1] += 60; // MT-32 mode
                    if (evidenceStrength < 60) {
                        sysExScore = 60; // Moderate SysEx evidence
                        evidenceStrength = 60;
                        sysExEvidence.append("Roland MT-32 SysEx (partial)");
                    }
                }
                // Partial Yamaha SysEx patterns (moderate evidence)
                else if (event.sysExData.size() >= 4 &&
                         event.sysExData[0] == 0x43 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x4C) {
                    modeScores[3] += 60; // XG mode
                    if (evidenceStrength < 60) {
                        sysExScore = 60; // Moderate SysEx evidence
                        evidenceStrength = 60;
                        sysExEvidence.append("Yamaha XG SysEx (partial)");
                    }
                }
                // MT-32 specific SysEx patterns (strong evidence)
                // MT-32 Patch Data: F0 41 10 16 12 05 xx xx ... (Patch memory)
                else if (event.sysExData.size() >= 6 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x16 && event.sysExData[3] == 0x12 &&
                         event.sysExData[4] == 0x05) {
                    modeScores[1] += 60; // Strong MT-32 evidence
                    if (evidenceStrength < 60) {
                        sysExScore = 60;
                        evidenceStrength = 60;
                        sysExEvidence.append("MT-32 Patch Data SysEx");
                    }
                }
                // MT-32 Display Text: F0 41 10 16 12 20 00 xx ... (Display message)
                else if (event.sysExData.size() >= 6 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x16 && event.sysExData[3] == 0x12 &&
                         event.sysExData[4] == 0x20 && event.sysExData[5] == 0x00) {
                    modeScores[1] += 65; // Strong MT-32 evidence
                    if (evidenceStrength < 65) {
                        sysExScore = 65;
                        evidenceStrength = 65;
                        sysExEvidence.append("MT-32 Display Text SysEx");
                    }
                }
                // MT-32 Rhythm Setup: F0 41 10 16 12 03 01 xx ... (Rhythm part)
                else if (event.sysExData.size() >= 6 &&
                         event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 &&
                         event.sysExData[2] == 0x16 && event.sysExData[3] == 0x12 &&
                         event.sysExData[4] == 0x03 && event.sysExData[5] == 0x01) {
                    modeScores[1] += 55; // Moderate MT-32 evidence
                    if (evidenceStrength < 55) {
                        sysExScore = 55;
                        evidenceStrength = 55;
                        sysExEvidence.append("MT-32 Rhythm Setup SysEx");
                    }
                }
                // Generic manufacturer SysEx (weak evidence)
                else if (event.sysExData.size() >= 3) {
                    if (event.sysExData[0] == 0x41 && event.sysExData[1] == 0x10 && evidenceStrength < 35) {
                        modeScores[2] += 35; // GS mode (Roland default)
                        if (evidenceStrength < 35) {
                            sysExScore = 35;
                            evidenceStrength = 35;
                            sysExEvidence.append("Roland SysEx (generic)");
                        }
                    }
                    else if (event.sysExData[0] == 0x43 && event.sysExData[1] == 0x10 && evidenceStrength < 35) {
                        modeScores[3] += 35; // XG mode
                        if (evidenceStrength < 35) {
                            sysExScore = 35;
                            evidenceStrength = 35;
                            sysExEvidence.append("Yamaha SysEx (generic)");
                        }
                    }
                }
            }
        }
    }

    // Analyze channel usage patterns for additional evidence
    QSet<int> usedChannels;
    bool hasChannel1Activity = false;
    bool hasChannel10Activity = false; // Drum channel
    bool hasHighChannelActivity = false; // Channels 11-16 (MT-32 doesn't support these)
    int noteOnCount = 0;
    QSet<int> mt32Programs; // Track MT-32 characteristic programs
    QSet<int> gmPrograms; // Track GM characteristic programs

    // Second pass: analyze channel usage patterns
    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (!event.isMetaEvent && !event.isSysExEvent) {
                unsigned char status = event.status;
                int channel = status & 0x0F; // Extract channel (0-15)

                // Track Note On events
                if ((status & 0xF0) == 0x90 && event.data2 > 0) { // Note On with velocity > 0
                    usedChannels.insert(channel);
                    noteOnCount++;

                    if (channel == 0) { // Channel 1 (0-based)
                        hasChannel1Activity = true;
                    }
                    if (channel == 9) { // Channel 10 (0-based)
                        hasChannel10Activity = true;
                    }
                    if (channel >= 10) { // Channels 11-16 (0-based: 10-15)
                        hasHighChannelActivity = true;
                    }
                }
                // Also check Program Change events for channel usage and program analysis
                else if ((status & 0xF0) == 0xC0) {
                    usedChannels.insert(channel);
                    int program = event.data1;

                    // Track high channel usage for non-MT-32 modes
                    if (channel >= 10) { // Channels 11-16 (0-based: 10-15)
                        hasHighChannelActivity = true;
                    }

                    // MT-32 characteristic programs (different from GM)
                    // MT-32 has unique sound assignments that differ from GM
                    if (program >= 64 && program <= 87) { // MT-32 Synth range
                        mt32Programs.insert(program);
                    }
                    else if (program >= 88 && program <= 127) { // MT-32 Analog/Seq range
                        mt32Programs.insert(program);
                    }

                    // GM characteristic programs (standard GM sounds)
                    if (program >= 0 && program <= 7) { // Piano family
                        gmPrograms.insert(program);
                    }
                    else if (program >= 40 && program <= 47) { // Violin family
                        gmPrograms.insert(program);
                    }
                    else if (program >= 56 && program <= 63) { // Trumpet family
                        gmPrograms.insert(program);
                    }
                }
            }
        }
    }

    // MT-32 specific channel pattern analysis
    if (noteOnCount >= 10) { // Only analyze if there's sufficient note activity
        // MT-32 typically doesn't use channel 1 (many files start from channel 2)
        if (!hasChannel1Activity && usedChannels.size() >= 2) {
            modeScores[1] += 25; // Restored MT-32 mode bonus
            textEvidence.append("MT-32 pattern: No channel 1 usage");
        }

        // MT-32 files often don't use drum channel (channel 10)
        if (!hasChannel10Activity && usedChannels.size() >= 3) {
            modeScores[1] += 15; // Additional MT-32 bonus
            textEvidence.append("MT-32 pattern: No drum channel usage");
        }

        // GM/GS files typically use channel 1 and often have drum channel
        if (hasChannel1Activity && hasChannel10Activity) {
            modeScores[0] += 20; // GM mode bonus
            modeScores[2] += 20; // GS mode bonus
            textEvidence.append("GM/GS pattern: Uses channel 1 and drums");
        }
        // GM files can also skip channel 1 but still use drums (modern GM files)
        else if (!hasChannel1Activity && hasChannel10Activity && usedChannels.size() >= 3) {
            modeScores[0] += 10; // Small GM mode bonus for drum usage without channel 1
            modeScores[2] += 10; // Small GS mode bonus
            textEvidence.append("GM/GS pattern: Uses drums without channel 1");
        }

        // Program Change analysis
        if (!mt32Programs.isEmpty()) {
            int mt32ProgramBonus = qMin(25, mt32Programs.size() * 5); // Max 25 points
            modeScores[1] += mt32ProgramBonus; // MT-32 mode bonus
            textEvidence.append(QString("MT-32 pattern: %1 MT-32 programs used").arg(mt32Programs.size()));
        }

        if (!gmPrograms.isEmpty()) {
            int gmProgramBonus = qMin(20, gmPrograms.size() * 4); // Max 20 points
            modeScores[0] += gmProgramBonus; // GM mode bonus
            modeScores[2] += gmProgramBonus; // GS mode bonus (GS includes GM)
            textEvidence.append(QString("GM/GS pattern: %1 GM programs used").arg(gmPrograms.size()));
        }

        // High channel usage (11-16) indicates non-MT-32 formats
        // MT-32 only supports 9 parts total (8 melody + 1 rhythm)
        if (hasHighChannelActivity) {
            modeScores[0] += 30; // Strong GM mode bonus
            modeScores[2] += 30; // Strong GS mode bonus
            modeScores[3] += 30; // Strong XG mode bonus
            textEvidence.append("Non-MT-32 pattern: Uses high channels (11-16)");
        }
    }

    // Calculate final confidence score using improved algorithm
    int finalScore;

    if (sysExScore > 0 && textScore > 0) {
        // Both types of evidence: weighted average with bonus
        finalScore = (sysExScore * 0.7) + (textScore * 0.5); // Can exceed 100
        finalScore += 10; // Bonus for having both types
        finalScore = qMin(98, finalScore); // Cap at 98
    } else if (sysExScore > 0) {
        // SysEx evidence only
        finalScore = sysExScore;
    } else if (textScore > 0) {
        // Text evidence only
        finalScore = textScore;
    } else {
        // No specific evidence found - use GM default
        finalScore = 25; // Slightly higher base score for GM
    }

    // Apply confidence adjustments based on detection mode
    if (reliability.detectedMode == 0) {
        finalScore = 25; // GM default - slightly uncertain but often correct
    } else if (finalScore < 30) {
        // Very weak evidence gets penalty
        finalScore = qMax(15, finalScore - 5);
    }

    // Determine detected mode based on highest score
    int maxScore = 0;
    int detectedMode = 0; // Default to GM
    for (auto it = modeScores.begin(); it != modeScores.end(); ++it) {
        if (it.value() > maxScore) {
            maxScore = it.value();
            detectedMode = it.key();
        }
    }
    reliability.detectedMode = detectedMode;

    // Calculate confidence score with proper normalization
    // Use a more sophisticated approach: strongest evidence + bonus for multiple evidence
    int confidenceScore = maxScore;

    // If there are multiple pieces of evidence, add small bonus but cap at 100
    int evidenceCount = 0;
    for (auto it = modeScores.begin(); it != modeScores.end(); ++it) {
        if (it.value() > 0 && it.key() == detectedMode) {
            evidenceCount = 1; // At least one evidence for detected mode
            break;
        }
    }

    // Add small bonus for very strong evidence, but cap at 100
    if (maxScore >= 95) {
        confidenceScore = qMin(100, maxScore + 0); // Perfect evidence stays at 95-100
    } else if (maxScore >= 75) {
        confidenceScore = qMin(95, maxScore + 5); // Strong evidence gets small boost
    } else {
        confidenceScore = maxScore; // Weaker evidence stays as is
    }

    reliability.confidenceScore = confidenceScore;
    reliability.hasStrongEvidence = (confidenceScore >= 80);

    // Store all mode scores for ranking
    reliability.allModeScores = modeScores;

    // Calculate the best alternative based on actual scores
    reliability.alternativeMode = -1;
    int bestAlternativeScore = 0;

    // Only show alternatives for low confidence
    if (confidenceScore < 60) {
        // Find the second highest scoring mode as alternative
        for (auto it = modeScores.begin(); it != modeScores.end(); ++it) {
            int mode = it.key();
            int score = it.value();

            // Skip the detected mode and find the highest scoring alternative
            if (mode != reliability.detectedMode && score > bestAlternativeScore) {
                reliability.alternativeMode = mode;
                bestAlternativeScore = score;
            }
        }

        // Only show if alternative has reasonable score (at least 15 points)
        if (bestAlternativeScore < 15) {
            reliability.alternativeMode = -1;
        }
    }

    reliability.confusionHint = getConfusionHint(reliability.alternativeMode, confidenceScore);

    // Build detection method description
    QStringList methods;
    if (sysExScore > 0) methods.append("SysEx");
    if (textScore > 0) methods.append("Text");
    if (methods.isEmpty()) methods.append("Default");

    reliability.detectionMethod = methods.join(" + ");

    // Build evidence list
    reliability.evidenceList.append(sysExEvidence);
    reliability.evidenceList.append(textEvidence);
    if (reliability.evidenceList.isEmpty()) {
        reliability.evidenceList.append("No specific mode indicators found");
    }

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
