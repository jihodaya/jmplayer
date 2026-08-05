#include "settingsmanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QSet>
#include <QDebug>

SettingsManager& SettingsManager::instance()
{
    static SettingsManager instance;
    return instance;
}

SettingsManager::SettingsManager()
{
    QString iniPath = createSettingsPath();
    m_settings = new QSettings(iniPath, QSettings::IniFormat);
}

SettingsManager::~SettingsManager()
{
    if (m_settings) {
        m_settings->sync();
        delete m_settings;
    }
}

// Volume a path sits on: "e:" for a drive, "//server/share" for a network path,
// empty when it cannot be told. Two paths on the same volume travel together, so
// a relative link between them survives a drive-letter change.
static QString volumeOf(const QString& path)
{
    const QString s = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (s.startsWith("//")) {
        const int a = s.indexOf('/', 2);
        if (a < 0) return s.toLower();
        const int b = s.indexOf('/', a + 1);
        return (b < 0 ? s : s.left(b)).toLower();
    }
    if (s.size() >= 2 && s[1] == QLatin1Char(':')) return s.left(2).toLower();
    return QString();
}

QString SettingsManager::storageDir()
{
    // Resolved once - see the header for why it must not be re-checked.
    static const QString dir = []() -> QString {
        const QString cfg = QCoreApplication::applicationDirPath() + "/cfg";
        if (QFileInfo(cfg).isDir()) {
            // Present but unwritable (read-only stick, Program Files) would lose
            // every setting silently, so prove it can be written to first.
            QTemporaryFile probe(cfg + "/.writetestXXXXXX");
            if (probe.open()) {
                probe.close();
                return cfg;
            }
            qWarning() << "[Settings] cfg folder is not writable, using Documents:" << cfg;
        }
        return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
               + "/JMPLAYER";
    }();
    return dir;
}

bool SettingsManager::isPortable()
{
    return storageDir() == QCoreApplication::applicationDirPath() + "/cfg";
}

QString SettingsManager::portableMusicDir()
{
    if (!isPortable()) return QString();

    const QString music = QCoreApplication::applicationDirPath() + "/Music";
    static bool created = false;
    if (!created) {
        created = true;
        if (!QFileInfo(music).isDir()) QDir().mkpath(music);
    }
    return music;
}

QString SettingsManager::toPortablePath(const QString& absolutePath)
{
    if (absolutePath.isEmpty() || !isPortable()) return absolutePath;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString abs = QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
    if (!QDir::isAbsolutePath(abs)) return absolutePath;      // already relative

    const QString vol = volumeOf(abs);
    if (vol.isEmpty() || vol != volumeOf(appDir)) return absolutePath;

    return QDir(appDir).relativeFilePath(abs);
}

QString SettingsManager::fromPortablePath(const QString& storedPath)
{
    if (storedPath.isEmpty() || QDir::isAbsolutePath(storedPath)) return storedPath;

    // Some of these keys also carry markers rather than paths - the playlist
    // stores "__PLAYLIST_ROOT__" to mean "was at the top". Resolving that
    // against the program folder produced a path that matched nothing, so the
    // remembered position was quietly lost every launch.
    if (storedPath.startsWith("__")) return storedPath;

    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/" + storedPath);
}

bool SettingsManager::isPathKey(const QString& key)
{
    static const QSet<QString> kPathKeys = {
        "General/lastOpenDirectory",
        "Synth/ExternalGybBank", "Synth/ExternalImsBank", "Synth/ExternalOkaBank",
        "Synth/SoundFontPath",   "Synth/SoundFontList",
        "browsingRootPath",      "currentFolderPath", "currentNodePath",
        "navigationHistory",
    };
    return kPathKeys.contains(key);
}

void SettingsManager::ensureStorageDir() const
{
    const QString dir = storageDir();
    if (!QFileInfo(dir).isDir()) QDir().mkpath(dir);
}

QString SettingsManager::createSettingsPath()
{
    return storageDir() + "/settings.ini";
}

void SettingsManager::setValue(const QString& key, const QVariant& value)
{
    ensureStorageDir();

    if (isPortable() && isPathKey(key)) {
        if (value.typeId() == QMetaType::QStringList) {
            QStringList out;
            const QStringList in = value.toStringList();
            out.reserve(in.size());
            for (const QString& p : in) out.append(toPortablePath(p));
            m_settings->setValue(key, out);
            return;
        }
        if (value.typeId() == QMetaType::QString) {
            m_settings->setValue(key, toPortablePath(value.toString()));
            return;
        }
    }
    m_settings->setValue(key, value);
}

QVariant SettingsManager::value(const QString& key, const QVariant& defaultValue) const
{
    const QVariant v = m_settings->value(key, defaultValue);
    if (!isPortable() || !isPathKey(key)) return v;

    // Older portable profiles hold absolute paths; fromPortablePath passes those
    // straight through, so both forms keep working.
    if (v.typeId() == QMetaType::QStringList) {
        QStringList out;
        const QStringList in = v.toStringList();
        out.reserve(in.size());
        for (const QString& p : in) out.append(fromPortablePath(p));
        return out;
    }
    if (v.typeId() == QMetaType::QString) return fromPortablePath(v.toString());
    return v;
}

void SettingsManager::remove(const QString& key)
{
    m_settings->remove(key);
}

void SettingsManager::sync()
{
    ensureStorageDir();
    m_settings->sync();
}

void SettingsManager::beginWriteArray(const QString& prefix, int size)
{
    ensureStorageDir();
    m_settings->beginWriteArray(prefix, size);
}

void SettingsManager::endArray()
{
    m_settings->endArray();
}

void SettingsManager::setArrayIndex(int i)
{
    m_settings->setArrayIndex(i);
}

int SettingsManager::beginReadArray(const QString& prefix)
{
    return m_settings->beginReadArray(prefix);
}