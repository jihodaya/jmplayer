#include "mldfmplayer.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include "jjomesynth.h"
#include <algorithm>
#include <cmath>

MldFmPlayer::MldFmPlayer(QObject* parent)
    : QObject(parent)
{
    m_fm.init(JJoMeSynth::instance().deviceSampleRate());
}

// The sample bank, if the song names one. Its recorded name is not always the
// name on disk - one song here asks for "ken.pdz" and what survives is
// "KEN.PDX" - and the two extensions hold the same ADPCM, so both are tried
// either way round in case.
static QByteArray loadSampleBank(const QString& songPath, const QByteArray& song)
{
    int i = 0;
    const int n = song.size();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(song.constData());
    while (i + 2 < n) {
        if (d[i] == 0x0D && d[i + 1] == 0x0A && d[i + 2] == 0x1A) { i += 3; break; }
        if (d[i] == 0x1A) { i += 1; break; }
        ++i;
    }
    QString name;
    while (i < n && d[i] != 0x00) name += QChar(d[i++]);
    if (name.isEmpty()) return QByteArray();

    const QString dir = QFileInfo(songPath).absolutePath() + "/";
    QStringList tries;
    tries << name;
    const int dot = name.lastIndexOf('.');
    if (dot < 0) { tries << name + ".pdx" << name + ".pdz"; }
    else {
        const QString stem = name.left(dot);
        tries << stem + ".pdx" << stem + ".pdz";
    }
    const QStringList base = tries;
    for (const QString& t : base) tries << t.toUpper() << t.toLower();

    for (const QString& t : tries) {
        QFile f(dir + t);
        if (f.open(QIODevice::ReadOnly)) return f.readAll();
    }
    return QByteArray();
}

bool MldFmPlayer::loadFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray song = f.readAll();
    f.close();
    if (song.size() < 32) return false;

    const QByteArray pdx = loadSampleBank(path, song);

    std::lock_guard<std::mutex> g(m_lock);
    m_playing.store(false, std::memory_order_relaxed);
    // THE DEVICE RATE, not a convenient constant. JJoMeSynth opens at 49716 -
    // the OPL3's own rate, chosen for the OPL engines - and this engine was
    // initialised at 44100, so its sequencer ran at 88.7% speed while the MIDI
    // half kept wall-clock time. The two drifted apart within seconds, which is
    // what "the sync is off and it sounds a mess" was.
    m_fm.init(JJoMeSynth::instance().deviceSampleRate());
    const bool ok = m_fm.load(reinterpret_cast<const uint8_t*>(song.constData()),
                              size_t(song.size()),
                              pdx.isEmpty() ? nullptr
                                            : reinterpret_cast<const uint8_t*>(pdx.constData()),
                              size_t(pdx.size()));
    m_fmTracks = ok ? m_fm.trackCount() : 0;
    m_loaded.store(ok, std::memory_order_release);
    return ok;
}

void MldFmPlayer::unload()
{
    std::lock_guard<std::mutex> g(m_lock);
    m_playing.store(false, std::memory_order_relaxed);
    m_loaded.store(false, std::memory_order_release);
    m_started = false;
    m_fm.stop();
}

void MldFmPlayer::play()
{
    if (!m_loaded.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_started) {
        m_fm.play();
        m_started = true;
    }
    m_playing.store(true, std::memory_order_release);
}

void MldFmPlayer::stop()
{
    std::lock_guard<std::mutex> g(m_lock);
    m_playing.store(false, std::memory_order_release);
    m_started = false;
    m_fm.stop();
}

void MldFmPlayer::pause()
{
    m_playing.store(false, std::memory_order_release);
}

void MldFmPlayer::seekMs(unsigned long ms)
{
    if (!m_loaded.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> g(m_lock);
    m_fm.seekMs(ms);
    m_started = true;
}

unsigned long MldFmPlayer::positionMs() const
{
    std::lock_guard<std::mutex> g(m_lock);
    return m_fm.positionMs();
}

void MldFmPlayer::setTempoScale(int percent)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_fm.setTempoScale(percent);
}

void MldFmPlayer::setTranspose(int semitones)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_fm.setTranspose(semitones);
}

void MldFmPlayer::setBalance(int percent)
{
    m_balance.store(std::max(0, std::min(100, percent)), std::memory_order_relaxed);
}

void MldFmPlayer::setVolume(int volume)
{
    m_volume.store(std::max(0, std::min(127, volume)), std::memory_order_relaxed);
}

// A centre detent that does not change anything: at 50 both halves play at the
// level their own files ask for, and each side only ever attenuates from there.
// Boosting instead would clip the half that is already loud - `defeat` measures
// +5.1 dB of FM over its MIDI as it stands.
float MldFmPlayer::fmGain() const
{
    const int b = m_balance.load(std::memory_order_relaxed);
    const float v = m_volume.load(std::memory_order_relaxed) / 127.0f;
    return (b >= 50) ? v : v * (b / 50.0f);
}

float MldFmPlayer::midiGain() const
{
    const int b = m_balance.load(std::memory_order_relaxed);
    return (b <= 50) ? 1.0f : ((100 - b) / 50.0f);
}

void MldFmPlayer::renderAudio(float* output, unsigned int frameCount)
{
    if (!output || frameCount == 0) return;
    if (!m_playing.load(std::memory_order_acquire)) return;

    std::unique_lock<std::mutex> g(m_lock, std::try_to_lock);
    if (!g.owns_lock()) return;                 // a load is in flight; stay silent
    if (!m_loaded.load(std::memory_order_relaxed)) return;

    const size_t need = size_t(frameCount) * 2;
    if (m_scratch.size() < need) m_scratch.resize(need);
    m_fm.render16(m_scratch.data(), int(frameCount));

    const float gain = fmGain() * (1.0f / 32768.0f);
    for (unsigned int i = 0; i < need; ++i)
        output[i] += m_scratch[i] * gain;

    if (!m_fm.isPlaying()) m_playing.store(false, std::memory_order_release);
}
