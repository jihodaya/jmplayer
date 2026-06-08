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

private:
    SettingsManager();
    ~SettingsManager();
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    QSettings* m_settings;

    // Helper to create settings path like original createSettings()
    QString createSettingsPath();
};

#endif // SETTINGSMANAGER_H