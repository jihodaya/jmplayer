#ifndef SOUNDFONTMANAGERDIALOG_H
#define SOUNDFONTMANAGERDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

class SoundFontManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SoundFontManagerDialog(QWidget *parent = nullptr);
    ~SoundFontManagerDialog();

private slots:
    void addSoundFont();
    void removeSoundFont();
    void applySoundFont();
    void onSelectionChanged();

private:
    void setupUI();
    void loadSettings();
    void saveSettings();
    void updateListUI();

    QListWidget *sfListWidget;
    QPushButton *addButton;
    QPushButton *removeButton;
    QPushButton *applyButton;
    QPushButton *closeButton;

    QString activeSoundFont;
};

#endif // SOUNDFONTMANAGERDIALOG_H
