#include "settingsmanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>

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

QString SettingsManager::createSettingsPath()
{
    // Replicate the exact logic from MainWindow::createSettings()
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString appDir = documentsPath + "/JMPLAYER";

    QDir dir;
    if (!dir.exists(appDir)) {
        dir.mkpath(appDir);
    }

    return appDir + "/settings.ini";
}

void SettingsManager::setValue(const QString& key, const QVariant& value)
{
    m_settings->setValue(key, value);
}

QVariant SettingsManager::value(const QString& key, const QVariant& defaultValue) const
{
    return m_settings->value(key, defaultValue);
}

void SettingsManager::remove(const QString& key)
{
    m_settings->remove(key);
}

void SettingsManager::sync()
{
    m_settings->sync();
}

void SettingsManager::beginWriteArray(const QString& prefix, int size)
{
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