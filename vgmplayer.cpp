#include "vgmplayer.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <vector>

namespace {

// Reads enough of a file to answer a question about its header. A .vgz has to
// be unpacked before the header can be read at all, and these files are small,
// so the whole thing is read either way.
bool readWhole(const QString& path, std::vector<uint8_t>& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = f.readAll();
    f.close();
    if (b.isEmpty()) return false;
    out.assign(reinterpret_cast<const uint8_t*>(b.constData()),
               reinterpret_cast<const uint8_t*>(b.constData()) + b.size());
    return true;
}

bool readPlainVgm(const QString& path, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> raw;
    if (!readWhole(path, raw)) return false;
    if (raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
        std::vector<uint8_t> plain;
        if (!VgmBackend::gunzip(raw, plain)) return false;
        out.swap(plain);
    } else {
        out.swap(raw);
    }
    return out.size() >= 0x40;
}

} // namespace


VgmPlayer::VgmPlayer(QObject* parent)
    : QObject(parent)
    , m_playing(false)
    , m_paused(false)
    , m_volume(100)
{
    // The audio device runs at the OPL3's rate throughout this player, so the
    // VGM chips are resampled to it rather than the other way round.
    m_vgm.init(49716);
}

VgmPlayer::~VgmPlayer()
{
    stop();
}

// A VGM naming only OPL chips keeps going to AdPlug: that path is old, tested,
// and covers 18 of the 36 files here. This one takes the rest.
bool VgmPlayer::handlesFile(const QString& fileName)
{
    const QString lower = fileName.toLower();
    if (!lower.endsWith(".vgm") && !lower.endsWith(".vgz")) return false;

    std::vector<uint8_t> d;
    if (!readPlainVgm(fileName, d)) return false;
    if (VgmBackend::wantsOnlyOpl(d.data(), d.size())) return false;

    VgmBackend probe;
    return probe.load(d.data(), d.size()) && probe.chips().find("no chip") == std::string::npos;
}

QString VgmPlayer::describeChips(const QString& fileName)
{
    std::vector<uint8_t> d;
    if (!readPlainVgm(fileName, d)) return QString();
    return QString::fromStdString(VgmBackend::describeChips(d.data(), d.size()));
}

QString VgmPlayer::extractTitleQuick(const QString& fileName)
{
    std::vector<uint8_t> d;
    if (!readPlainVgm(fileName, d)) return QString();

    VgmBackend probe;
    if (!probe.load(d.data(), d.size())) return QString();

    // GD3 carries the track name and the game it came from; both together is
    // what a playlist row wants.
    const QString track = QString::fromStdString(probe.title()).trimmed();
    const QString game  = QString::fromStdString(probe.game()).trimmed();
    if (track.isEmpty()) return game;
    if (game.isEmpty()) return track;
    return track + " (" + game + ")";
}

bool VgmPlayer::loadFile(const QString& fileName)
{
    stop();

    std::vector<uint8_t> d;
    if (!readPlainVgm(fileName, d)) {
        qWarning() << "[VgmPlayer] cannot read" << fileName;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vgm.load(d.data(), d.size())) {
        qWarning() << "[VgmPlayer] not a VGM this build plays:" << fileName;
        return false;
    }

    m_currentFile = fileName;
    m_duration = m_vgm.getTotalMs();
    if (m_duration == 0) m_duration = 180000;
    m_vgm.setVolume(m_volume.load(std::memory_order_relaxed) / 127.0f);

    m_title = QString::fromStdString(m_vgm.title()).trimmed();
    if (m_title.isEmpty()) m_title = QFileInfo(fileName).completeBaseName();

    // One monitor row per chip voice; a VGM has no instrument names, so the
    // rows are the chip's own channel names.
    m_instruments.clear();
    for (int i = 0; i < m_vgm.voiceCount(); ++i) {
        m_instruments.append(QString::fromStdString(m_vgm.voiceName(i)));
    }

    qDebug() << "[VgmPlayer] loaded" << m_title
             << "chips" << QString::fromStdString(m_vgm.chips())
             << "duration" << m_duration << "ms";
    return true;
}

void VgmPlayer::play()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vgm.play();
    m_playing.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
}

void VgmPlayer::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vgm.pause();
    m_paused.store(true, std::memory_order_release);
}

void VgmPlayer::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vgm.stop();
    m_playing.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
}

void VgmPlayer::setPosition(unsigned long positionMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vgm.seekMs(static_cast<uint32_t>(positionMs));
    if (m_playing.load(std::memory_order_acquire)) m_vgm.play();
}

unsigned long VgmPlayer::getPosition() const
{
    return m_vgm.getElapsedMs();
}

void VgmPlayer::setVolume(int volume)
{
    const int v = std::clamp(volume, 0, 127);
    m_volume.store(v, std::memory_order_relaxed);
    m_vgm.setVolume(v / 127.0f);
}

// Polled from the GUI thread while the audio thread renders. Reading a few
// ints out of the chip state is harmless - a stale bar for one frame is the
// worst it can do - and taking the render lock for a display would stall audio.
QList<int> VgmPlayer::getVoiceVolumes() const
{
    QList<int> vols;
    const int n = m_vgm.voiceCount();
    for (int i = 0; i < 20; ++i) vols.append(i < n ? m_vgm.voiceLevel(i) : 0);
    return vols;
}

QStringList VgmPlayer::getVoiceInstrumentNames() const
{
    // The monitor parses a row as `name|note|volume|on`. This used to send the
    // name alone, so the note column stayed blank and the volume column read 0
    // on every VGM while the bars moved - the OPL engines have sent all four
    // fields all along.
    //
    // The note field is left empty deliberately. Half these voices are PCM
    // channels - SegaPCM, QSound, the YM2610's ADPCM - which have a playback
    // rate and no pitch, so there is no note to name. Deriving one from the FM
    // and PSG registers is a per-chip job and is not done here.
    QStringList names;
    const int n = m_vgm.voiceCount();
    for (int i = 0; i < 20; ++i) {
        if (i >= n) { names.append(QString()); continue; }
        const int vol = m_vgm.voiceLevel(i);
        names.append(QString("%1|%2|%3|%4")
                         .arg(QString::fromStdString(m_vgm.voiceName(i)))
                         .arg(QString())
                         .arg(vol)
                         .arg(vol > 0 ? "1" : "0"));
    }
    return names;
}

QList<int> VgmPlayer::getInstrumentVolumes() const
{
    QList<int> vols;
    for (int i = 0; i < m_instruments.size(); ++i) vols.append(m_vgm.voiceLevel(i));
    return vols;
}

QString VgmPlayer::getBankName() const
{
    return QString::fromStdString(m_vgm.chips());
}

QString VgmPlayer::getChips() const
{
    return QString::fromStdString(m_vgm.chips());
}


// One pole and a soft-clip, lifted from ImsPlayer so the button means the same
// thing whichever engine is playing.
void VgmPlayer::setStereoMode(int mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vgm.setStereoMode(mode);
}

void VgmPlayer::setDspLevel(int level)
{
    m_dspLevel.store((level < 0 || level > 3) ? 0 : level, std::memory_order_relaxed);
}

namespace {
void applyAnalogDsp(float* out, unsigned int frames, int level, float* lpfLast)
{
    if (level <= 0) return;
    float alpha = 1.0f, drive = 1.0f;
    if (level == 1)      { alpha = 0.80f; drive = 1.05f; }
    else if (level == 2) { alpha = 0.55f; drive = 1.30f; }
    else                 { alpha = 0.35f; drive = 1.65f; }

    auto softClip = [](float x) {
        if (x > 1.0f) return 1.0f;
        if (x < -1.0f) return -1.0f;
        return x - (x * x * x) / 3.0f;
    };

    for (unsigned int i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            float v = out[i * 2 + c];
            v = alpha * v + (1.0f - alpha) * lpfLast[c];
            lpfLast[c] = v;
            out[i * 2 + c] = softClip(v * drive);
        }
    }
}
} // namespace

void VgmPlayer::renderAudio(float* output, unsigned int frameCount)
{
    if (!m_playing.load(std::memory_order_acquire) || m_paused.load(std::memory_order_acquire))
        return;

    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_vgm.isPlaying()) {
            m_vgm.render(output, int(frameCount));
        }
        // The backend stops itself when the stream ends, and this is the only
        // place that notices in time to move the playlist on.
        if (!m_vgm.isPlaying()) {
            m_playing.store(false, std::memory_order_release);
            ended = true;
        }
    }

    applyAnalogDsp(output, frameCount, m_dspLevel.load(std::memory_order_relaxed), m_lpfLast);

    // Queued by name and outside the lock - see the same note in mdxplayer.cpp.
    if (ended) QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}
