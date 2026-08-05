#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QSettings>
#include <QString>
#include <QVariant>
#include <QStandardPaths>
#include <QDir>

class SettingsManager
{
public:
    static SettingsManager& instance();

    void setValue(const QString& key, const QVariant& value);
    QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const;
    void remove(const QString& key);
    void sync();

    // Array operations for compatibility
    void beginWriteArray(const QString& prefix, int size = -1);
    void endArray();
    void setArrayIndex(int i);
    int beginReadArray(const QString& prefix);

    // ---- Where this copy keeps its settings --------------------------------
    // A "cfg" folder next to the executable turns on portable mode: settings and
    // playlist are written there and My Documents is left alone. Neither the
    // program nor the build ever creates that folder - the user creating it is
    // the entire signal, so an ordinary install behaves exactly as before, and
    // deleting it sends the next run back to My Documents.
    //
    // Resolved once per run and cached, so settings and playlist can never end
    // up split between the two because the folder appeared or vanished mid-run.
    static QString storageDir();
    static bool    isPortable();

    // Music that came with a portable copy lives here. Created only in portable
    // mode - an ordinary install should not grow folders it never uses.
    static QString portableMusicDir();

    // A USB stick is E: on one machine and F: on the next, so an absolute path
    // saved at home is dead at the office. That, not the settings location, is
    // what made portable use painful: the playlist kept full paths and every
    // entry died on a drive-letter change. In portable mode paths on the same
    // volume as the executable are therefore stored relative to it. Both calls
    // are pass-throughs otherwise, so a normal install writes the same settings
    // file it always did.
    static QString toPortablePath(const QString& absolutePath);
    static QString fromPortablePath(const QString& storedPath);

private:
    SettingsManager();
    ~SettingsManager();
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    QSettings* m_settings;

    // The storage folder is created on first write rather than at startup:
    // launching the player once used to leave a My Documents folder behind even
    // when nothing was ever saved, which is exactly the trace a user running
    // from USB does not want on someone else's machine.
    void ensureStorageDir() const;

    static bool isPathKey(const QString& key);

    // Helper to create settings path like original createSettings()
    QString createSettingsPath();
};

#endif // SETTINGSMANAGER_H
