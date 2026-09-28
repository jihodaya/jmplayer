#include "mdxplayer.h"
#include <windows.h>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <algorithm>
#if defined(JMP_HAVE_QZIP)
#include <QtCore/private/qzipreader_p.h>

#endif

namespace {
// MDX titles are Shift-JIS. Decoding them here rather than through the player's
// general text decoder keeps this file free of the MIDI stack, which the audio
// test harnesses do not link.
QString decodeMdxText(const QByteArray& raw)
{
    if (raw.isEmpty()) return QString();

    bool ascii = true;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x80) { ascii = false; break; }
    }
    if (ascii) return QString::fromLatin1(raw);

    const int len = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                        raw.constData(), raw.size(), nullptr, 0);
    if (len > 0) {
        std::wstring buf(len, L'\0');
        if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                raw.constData(), raw.size(), buf.data(), len) > 0) {
            return QString::fromWCharArray(buf.data(), len);
        }
    }
    return QString::fromLatin1(raw);
}

// A .mdz is not necessarily an archive. The only one in the library here is a
// plain MDX that happens to carry that extension, and treating the extension as
// proof of a zip meant it would not load at all. Ask the file, not its name.
bool mdzIsArchive(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray magic = f.read(7);
    f.close();
    if (magic.startsWith("PK")) return true;
    // LZH is the other thing an X68000 archive is likely to be: its method id
    // sits at offset 2, after the header size and its checksum.
    if (magic.size() >= 7 && magic.mid(2, 3) == "-lh") {
        qWarning() << "[MdxPlayer] .mdz is an LZH archive, which is not read yet";
        return true;
    }
    return false;
}
} // namespace

MdxPlayer::MdxPlayer(QObject *parent)
    : QObject(parent)
    , m_playing(false)
    , m_paused(false)
    , m_volume(100)
    , m_tempoScale(100)
    , m_keyTranspose(0)
    , m_duration(180000) // Default 3 minutes (2 loops)
{
    m_mxdrv.init(49716);
}

MdxPlayer::~MdxPlayer()
{
    stop();
}

bool MdxPlayer::loadFile(const QString& fileName)
{
    stop();

    QFileInfo fi(fileName);
    if (!fi.exists()) {
        qWarning() << "[MdxPlayer] File does not exist:" << fileName;
        return false;
    }

    m_currentFile = fileName;
    QString ext = fi.suffix().toLower();

    // 1. Handle MDZ (ZIP Archive containing MDX and PDX)
#if defined(JMP_HAVE_QZIP)
    if (ext == "mdz" && mdzIsArchive(fileName)) {
        QZipReader zip(fileName);
        if (!zip.isReadable()) {
            qWarning() << "[MdxPlayer] Cannot open MDZ archive:" << fileName;
            return false;
        }

        QByteArray mdxBytes;
        QByteArray pdxBytes;

        const auto fileList = zip.fileInfoList();
        for (const auto& entry : fileList) {
            QString entryName = entry.filePath.toLower();
            if (entryName.endsWith(".mdx") && mdxBytes.isEmpty()) {
                mdxBytes = zip.fileData(entry.filePath);
            } else if (entryName.endsWith(".pdx") && pdxBytes.isEmpty()) {
                pdxBytes = zip.fileData(entry.filePath);
            }
        }
        zip.close();

        if (mdxBytes.isEmpty()) {
            qWarning() << "[MdxPlayer] No MDX file found inside MDZ:" << fileName;
            return false;
        }

        return loadData(mdxBytes, pdxBytes, fileName);
    }
#else
    if (ext == "mdz" && mdzIsArchive(fileName)) {
        qWarning() << "[MdxPlayer] .mdz needs the archive reader, which this build lacks";
        return false;
    }
#endif

    // 2. Handle Standalone MDX
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[MdxPlayer] Cannot open MDX file:" << fileName;
        return false;
    }
    QByteArray mdxBytes = file.readAll();
    file.close();

    // Fast header parse to find required PDX filename
    QByteArray pdxBytes;
    {
        Mxdrv tmp;
        if (tmp.loadMdx(reinterpret_cast<const uint8_t*>(mdxBytes.constData()), mdxBytes.size())) {
            std::string pdxName = tmp.getPdxName();
            if (!pdxName.empty()) {
                QString qPdxName = QString::fromStdString(pdxName).trimmed();
                if (!qPdxName.isEmpty()) {
                    // Most of the corpus names its sample bank without an
                    // extension - 126 of the songs here ask for "ssf2_x68" and
                    // the file beside them is "ssf2_x68.pdx". Looking for the
                    // literal string found the data for only 32 of 101.
                    if (!qPdxName.contains('.')) qPdxName += ".pdx";

                    // Search candidates for PDX:
                    // 1) Same folder (case-insensitive)
                    // 2) pdx/ subfolder
                    // 3) ApplicationDirPath/pdx/
                    // 4) ApplicationDirPath/SoundFonts/
                    QStringList searchDirs;
                    searchDirs << fi.absolutePath();
                    searchDirs << fi.absolutePath() + "/pdx";
                    searchDirs << QCoreApplication::applicationDirPath() + "/pdx";
                    searchDirs << QCoreApplication::applicationDirPath() + "/SoundFonts";
                    searchDirs << QCoreApplication::applicationDirPath();

                    // Failing that, the sample bank beside the song under
                    // the song's own name. DK_03.MDX asks for "dokyu.pdx" and
                    // what sits next to it is "DK_03.PDX" - whoever collected
                    // these renamed the pair together. Without this its drum
                    // track is silent, which is most of "the cymbal is wrong".
                    QStringList wanted;
                    wanted << qPdxName << (fi.completeBaseName() + ".pdx");

                    for (const QString& dirPath : searchDirs) {
                        QDir dir(dirPath);
                        if (!dir.exists()) continue;

                        // Case-insensitive search
                        const QStringList matches = dir.entryList(QStringList() << "*.pdx", QDir::Files);
                        for (const QString& want : wanted) {
                            for (const QString& match : matches) {
                                if (match.compare(want, Qt::CaseInsensitive) != 0) continue;
                                QFile pdxFile(dir.filePath(match));
                                if (pdxFile.open(QIODevice::ReadOnly)) {
                                    pdxBytes = pdxFile.readAll();
                                    pdxFile.close();
                                    qDebug() << "[MdxPlayer] Found PDX sample file:" << dir.filePath(match)
                                             << "(" << pdxBytes.size() << "bytes )";
                                    break;
                                }
                            }
                            if (!pdxBytes.isEmpty()) break;
                        }
                        if (!pdxBytes.isEmpty()) break;
                    }
                }
            }
        }
    }

    return loadData(mdxBytes, pdxBytes, fileName);
}

bool MdxPlayer::loadData(const QByteArray& mdxData, const QByteArray& pdxData, const QString& fileName)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (mdxData.isEmpty()) return false;

    const uint8_t* mdxPtr = reinterpret_cast<const uint8_t*>(mdxData.constData());
    const uint8_t* pdxPtr = pdxData.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(pdxData.constData());

    if (!m_mxdrv.loadMdx(mdxPtr, mdxData.size(), pdxPtr, pdxData.size())) {
        qWarning() << "[MdxPlayer] Failed to parse MDX data";
        return false;
    }

    // Decode Title using JMP's robust multi-encoding detector
    std::string rawTitle = m_mxdrv.getTitle();
    if (!rawTitle.empty()) {
        m_title = decodeMdxText(QByteArray(rawTitle.c_str(), static_cast<int>(rawTitle.length()))).trimmed();
    } else {
        m_title = QFileInfo(fileName).completeBaseName();
    }

    m_pdxName = QString::fromStdString(m_mxdrv.getPdxName());

    m_instruments.clear();
    for (int n : m_mxdrv.voiceNumbers()) {
        const QString name = QString("@%1").arg(n);
        if (!m_instruments.contains(name)) m_instruments.append(name);
    }
    // Measured by playing the song through with the chip silent, rather than
    // the flat three minutes this used to report.
    m_duration = m_mxdrv.getTotalMs();
    if (m_duration == 0) m_duration = 180000;

    m_mxdrv.setVolume(m_volume.load(std::memory_order_relaxed) / 127.0f);
    m_mxdrv.setTempoScale(m_tempoScale.load(std::memory_order_relaxed));
    m_mxdrv.setKeyTranspose(m_keyTranspose.load(std::memory_order_relaxed));

    qDebug() << "[MdxPlayer] MDX loaded successfully. Title:" << m_title << "PDX:" << m_pdxName;
    return true;
}

void MdxPlayer::play()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mxdrv.play();
    m_playing.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
}

void MdxPlayer::pause()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mxdrv.pause();
    m_paused.store(true, std::memory_order_release);
}

void MdxPlayer::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mxdrv.stop();
    m_playing.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
}

void MdxPlayer::setPosition(unsigned long positionMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mxdrv.seekMs(static_cast<uint32_t>(positionMs));
    if (m_playing.load(std::memory_order_acquire)) {
        m_mxdrv.play();
    }
}

unsigned long MdxPlayer::getPosition() const
{
    return m_mxdrv.getElapsedMs();
}

void MdxPlayer::setVolume(int volume)
{
    int vol = std::clamp(volume, 0, 127);
    m_volume.store(vol, std::memory_order_relaxed);
    m_mxdrv.setVolume(vol / 127.0f);
}

void MdxPlayer::setUserTempoScale(int scale)
{
    int s = std::clamp(scale, 50, 150);
    m_tempoScale.store(s, std::memory_order_relaxed);
    m_mxdrv.setTempoScale(s);
}

int MdxPlayer::getUserTempoScale() const
{
    return m_tempoScale.load(std::memory_order_relaxed);
}

void MdxPlayer::setStereoMode(int mode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_mxdrv.setStereoMode(mode);
}

void MdxPlayer::setUserKeyTranspose(int semitones)
{
    int k = std::clamp(semitones, -6, 6);
    m_keyTranspose.store(k, std::memory_order_relaxed);
    m_mxdrv.setKeyTranspose(k);
}

int MdxPlayer::getUserKeyTranspose() const
{
    return m_keyTranspose.load(std::memory_order_relaxed);
}


// The monitor polls these from the GUI thread while the audio thread is inside
// render(). Reading a few plain ints out of the track table is harmless - the
// worst a race can do is show one frame of a stale bar - and taking the render
// mutex here would stall audio for the sake of a display.
QList<int> MdxPlayer::getVoiceVolumes() const
{
    QList<int> vols;
    for (int i = 0; i < 20; ++i) {
        vols.append(i < m_mxdrv.trackCount() ? m_mxdrv.voiceLevel(i) : 0);
    }
    return vols;
}

QStringList MdxPlayer::getVoiceInstrumentNames() const
{
    QStringList names;
    for (int i = 0; i < 20; ++i) {
        const int v = (i < m_mxdrv.trackCount()) ? m_mxdrv.voiceNumber(i) : -1;
        names.append(v >= 0 ? QString("@%1").arg(v) : QString());
    }
    return names;
}

QList<int> MdxPlayer::getInstrumentVolumes() const
{
    QList<int> vols;
    for (int i = 0; i < m_instruments.size(); ++i) vols.append(0);
    for (int t = 0; t < m_mxdrv.trackCount(); ++t) {
        const int v = m_mxdrv.voiceNumber(t);
        if (v < 0) continue;
        const int idx = m_instruments.indexOf(QString("@%1").arg(v));
        if (idx < 0) continue;
        const int lvl = m_mxdrv.voiceLevel(t);
        if (lvl > vols[idx]) vols[idx] = lvl;
    }
    return vols;
}

QString MdxPlayer::getBankName() const
{
    return m_pdxName.isEmpty() ? QString("MDX") : m_pdxName;
}


// One pole and a soft-clip, lifted from ImsPlayer so the button means the same
// thing whichever engine is playing.
void MdxPlayer::setDspLevel(int level)
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

void MdxPlayer::renderAudio(float* output, unsigned int frameCount)
{
    if (!m_playing.load(std::memory_order_acquire) || m_paused.load(std::memory_order_acquire)) {
        return;
    }

    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mxdrv.isPlaying()) {
            m_mxdrv.render(output, frameCount);
        } else {
            m_playing.store(false, std::memory_order_release);
            ended = true;
        }
    }

    applyAnalogDsp(output, frameCount, m_dspLevel.load(std::memory_order_relaxed), m_lpfLast);

    // Announced the way GybPlayer already announces it, and outside the lock.
    //
    // A plain `emit` from the audio thread is only queued if Qt decides the
    // connection is cross-thread, and the handler's first act is to stop this
    // player - which takes the very lock the emit was made under. One
    // non-recursive std::mutex later, the window stops responding. Asking for a
    // queued call by name settles it rather than relying on that decision.
    if (ended) QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}

QString MdxPlayer::extractTitleQuick(const QString& fileName)
{
    QFileInfo fi(fileName);
    QString ext = fi.suffix().toLower();

#if defined(JMP_HAVE_QZIP)
    if (ext == "mdz" && mdzIsArchive(fileName)) {
        QZipReader zip(fileName);
        if (zip.isReadable()) {
            const auto fileList = zip.fileInfoList();
            for (const auto& entry : fileList) {
                if (entry.filePath.toLower().endsWith(".mdx")) {
                    QByteArray mdxBytes = zip.fileData(entry.filePath);
                    zip.close();
                    if (!mdxBytes.isEmpty()) {
                        Mxdrv tmp;
                        if (tmp.loadMdx(reinterpret_cast<const uint8_t*>(mdxBytes.constData()), mdxBytes.size())) {
                            std::string raw = tmp.getTitle();
                            if (!raw.empty()) {
                                return decodeMdxText(QByteArray(raw.c_str(), static_cast<int>(raw.length()))).trimmed();
                            }
                        }
                    }
                    break;
                }
            }
            zip.close();
        }
        return fi.completeBaseName();
    }
#endif

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return fi.completeBaseName();
    QByteArray header = file.read(512);
    file.close();

    Mxdrv tmp;
    if (tmp.loadMdx(reinterpret_cast<const uint8_t*>(header.constData()), header.size())) {
        std::string raw = tmp.getTitle();
        if (!raw.empty()) {
            return decodeMdxText(QByteArray(raw.c_str(), static_cast<int>(raw.length()))).trimmed();
        }
    }

    return fi.completeBaseName();
}
