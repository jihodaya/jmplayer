//
// midiresetdialog.h
//
// Settings dialog for the "reset the sound module before each new song"
// feature (see midireset.h). Modeless-style modal: the user picks whether to
// reset, which reset messages to send, and an optional inter-message delay for
// slow hardware. Reads/writes the same SettingsManager keys the MainWindow
// load path uses (Midi/ResetEnabled, Midi/ResetFlags, Midi/ResetDelayMs) and
// applies them live to the given MidiReset instance on accept.
//
#ifndef MIDIRESETDIALOG_H
#define MIDIRESETDIALOG_H

#include <QDialog>

class QCheckBox;
class QSpinBox;
class MidiReset;

class MidiResetDialog : public QDialog {
    Q_OBJECT
public:
    // pReset is applied live (and its values seed the controls); may be null,
    // in which case the dialog only persists settings.
    explicit MidiResetDialog(MidiReset* pReset, QWidget* parent = nullptr);

private slots:
    void applyAndAccept();

private:
    void setupUi();
    void syncEnabledState();

    MidiReset* m_pReset;

    QCheckBox* m_enable;
    QCheckBox* m_gm;
    QCheckBox* m_gs;
    QCheckBox* m_xg;
    QCheckBox* m_mt32;
    QSpinBox*  m_delay;
};

#endif // MIDIRESETDIALOG_H
