#include "soundfontmanagerdialog.h"
#include "settingsmanager.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QFileInfo>

SoundFontManagerDialog::SoundFontManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUI();
    loadSettings();
}

SoundFontManagerDialog::~SoundFontManagerDialog()
{
}

void SoundFontManagerDialog::setupUI()
{
    setWindowTitle("SoundFont Manager");
    setMinimumSize(400, 300);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QLabel *infoLabel = new QLabel("Select a SoundFont (.sf2) file to use with JJoMe Synth:", this);
    mainLayout->addWidget(infoLabel);

    sfListWidget = new QListWidget(this);
    mainLayout->addWidget(sfListWidget);
    
    connect(sfListWidget, &QListWidget::itemSelectionChanged, this, &SoundFontManagerDialog::onSelectionChanged);
    connect(sfListWidget, &QListWidget::itemDoubleClicked, this, &SoundFontManagerDialog::applySoundFont);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    addButton = new QPushButton("Add", this);
    removeButton = new QPushButton("Remove", this);
    applyButton = new QPushButton("Set Active", this);
    closeButton = new QPushButton("Close", this);

    removeButton->setEnabled(false);
    applyButton->setEnabled(false);

    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(removeButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(applyButton);
    buttonLayout->addWidget(closeButton);

    mainLayout->addLayout(buttonLayout);

    connect(addButton, &QPushButton::clicked, this, &SoundFontManagerDialog::addSoundFont);
    connect(removeButton, &QPushButton::clicked, this, &SoundFontManagerDialog::removeSoundFont);
    connect(applyButton, &QPushButton::clicked, this, &SoundFontManagerDialog::applySoundFont);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    // Dark theme styling
    setStyleSheet(
        "QDialog { background-color: #2b2b2b; color: #ffffff; }"
        "QLabel { color: #ffffff; }"
        "QListWidget { background-color: #1e1e1e; color: #ffffff; border: 1px solid #555555; }"
        "QListWidget::item:selected { background-color: #0078d4; }"
        "QPushButton { background-color: #3a3a3a; color: #ffffff; border: 1px solid #555555; padding: 5px; min-width: 70px; }"
        "QPushButton:hover { background-color: #4a4a4a; border: 1px solid #0078d4; }"
        "QPushButton:pressed { background-color: #2a2a2a; }"
        "QPushButton:disabled { color: #777777; background-color: #2b2b2b; border: 1px solid #444444; }"
    );
}

void SoundFontManagerDialog::loadSettings()
{
    SettingsManager& settings = SettingsManager::instance();
    QStringList sfList = settings.value("Synth/SoundFontList", QStringList()).toStringList();
    activeSoundFont = QDir::cleanPath(settings.value("Synth/SoundFontPath", "").toString());

    sfListWidget->clear();
    for (const QString& sfPathRaw : sfList) {
        QString sfPath = QDir::cleanPath(sfPathRaw);
        if (QFileInfo::exists(sfPath)) {
            QListWidgetItem* item = new QListWidgetItem(sfPath, sfListWidget);
            if (sfPath.compare(activeSoundFont, Qt::CaseInsensitive) == 0) {
                item->setText(QString("[Active] %1").arg(QFileInfo(sfPath).fileName()));
                item->setData(Qt::UserRole, sfPath);
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                item->setForeground(QColor("#00FF00"));
            } else {
                item->setText(QFileInfo(sfPath).fileName());
                item->setData(Qt::UserRole, sfPath);
            }
        }
    }
}

void SoundFontManagerDialog::saveSettings()
{
    SettingsManager& settings = SettingsManager::instance();
    
    QStringList sfList;
    for (int i = 0; i < sfListWidget->count(); ++i) {
        sfList.append(sfListWidget->item(i)->data(Qt::UserRole).toString());
    }
    
    settings.setValue("Synth/SoundFontList", sfList);
    settings.setValue("Synth/SoundFontPath", activeSoundFont);
    settings.sync();
}

void SoundFontManagerDialog::addSoundFont()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select SoundFont Files", "", "SoundFont Files (*.sf2)");
    if (files.isEmpty()) return;

    bool added = false;
    for (const QString& fileRaw : files) {
        QString file = QDir::cleanPath(fileRaw);
        // Check if already in list
        bool exists = false;
        for (int i = 0; i < sfListWidget->count(); ++i) {
            QString existingPath = QDir::cleanPath(sfListWidget->item(i)->data(Qt::UserRole).toString());
            if (existingPath.compare(file, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        
        if (!exists) {
            QListWidgetItem* item = new QListWidgetItem(QFileInfo(file).fileName(), sfListWidget);
            item->setData(Qt::UserRole, file);
            added = true;
        }
    }
    
    if (added) {
        saveSettings();
        // If it's the first one, make it active
        if (activeSoundFont.isEmpty() && sfListWidget->count() > 0) {
            sfListWidget->setCurrentRow(0);
            applySoundFont();
        }
    }
}

void SoundFontManagerDialog::removeSoundFont()
{
    QListWidgetItem* item = sfListWidget->currentItem();
    if (!item) return;
    
    QString path = item->data(Qt::UserRole).toString();

    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "Remove SoundFont", 
                                  "Are you sure you want to remove this SoundFont from the list?", 
                                  QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No) {
        return;
    }
    if (path == activeSoundFont) {
        activeSoundFont = "";
    }
    
    delete item;
    saveSettings();
    updateListUI();
}

void SoundFontManagerDialog::applySoundFont()
{
    QListWidgetItem* item = sfListWidget->currentItem();
    if (!item) return;
    
    activeSoundFont = item->data(Qt::UserRole).toString();
    saveSettings();
    updateListUI();
    
    QMessageBox::information(this, "SoundFont Set", "SoundFont has been set as active.\nIt will be used when '[JJoMe Synth (SoundFont)]' is selected as the device.");
}

void SoundFontManagerDialog::onSelectionChanged()
{
    bool hasSelection = sfListWidget->currentItem() != nullptr;
    removeButton->setEnabled(hasSelection);
    applyButton->setEnabled(hasSelection);
}

void SoundFontManagerDialog::updateListUI()
{
    for (int i = 0; i < sfListWidget->count(); ++i) {
        QListWidgetItem* item = sfListWidget->item(i);
        QString path = item->data(Qt::UserRole).toString();
        
        if (path == activeSoundFont) {
            item->setText(QString("[Active] %1").arg(QFileInfo(path).fileName()));
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            item->setForeground(QColor("#00FF00"));
        } else {
            item->setText(QFileInfo(path).fileName());
            QFont f = item->font();
            f.setBold(false);
            item->setFont(f);
            item->setForeground(QColor("#FFFFFF"));
        }
    }
}
