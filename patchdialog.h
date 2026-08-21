// F5: the song's instrument list, with what each OPL slot becomes on a MIDI
// module and a way to change it while the song plays.
//
// GAYOBANG's `악기변경` screen listed the slots and let you pick a tone for each
// one during playback. This is that screen. What it adds is the middle column:
// the DOS program started every slot at tone 1 and told you nothing, whereas
// here each row says where its current value came from - the song file itself,
// a name match, or nothing at all - because those three deserve very different
// amounts of attention. Sorting by note count puts the instruments that carry
// the song at the top.
#pragma once

#include <QDialog>
#include <QVector>

#include "gybokamidi.h"

class QTableWidget;
class QLabel;
class QCheckBox;
class QPushButton;

class PatchDialog : public QDialog {
    Q_OBJECT

public:
    PatchDialog(const QString& songPath, const QVector<gybokamidi::Row>& plan,
                QWidget* parent = nullptr);

    QVector<gybokamidi::Row> plan() const { return m_plan; }

    // True once the user has pressed 저장 / Save. Until then nothing is written
    // to disk - edits are applied to the sound and kept in memory only, so
    // browsing a song's instruments never leaves a file beside it.
    bool saved() const { return m_saved; }

signals:
    // Emitted as soon as a row changes so the sound follows the mouse rather
    // than waiting for OK - the whole point of doing this during playback.
    void assignmentChanged(int slot, const gybokamidi::Row& row);

protected:
    // Combo boxes inside a table swallow the wheel and change their value, so
    // scrolling the list silently reassigns whatever the pointer passed over.
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void save();
    void onDrumToggled(int row, bool drum);
    void onTargetChanged(int row, int index);
    void revertRow(int row);
    void revertAll();
    void onlyUnmatchedToggled(bool on);
    void setTarget(gybokamidi::Target target);

private:
    void buildTable();
    void fillTargetCombo(int row);
    void updateTargetButton();
    void refreshRow(int row);
    void applyFilter();

    QString                  m_songPath;
    QVector<gybokamidi::Row> m_plan;
    QTableWidget*            m_table = nullptr;
    QLabel*                  m_summary = nullptr;
    QCheckBox*               m_onlyUnmatched = nullptr;
    QPushButton*             m_saveButton = nullptr;
    QPushButton*             m_revertAll = nullptr;
    QLabel*                  m_hint = nullptr;

    // GM or MT-32. Switching rebuilds every instrument list, because the two
    // are different sets of instruments rather than two namings of one - see
    // gybokamidi.h. Both choices are kept per row, so flipping back and forth
    // loses nothing.
    //
    // Two buttons rather than one that changes its label: a single toggle never
    // says whether it is showing the current state or the thing it would switch
    // to. With both on screen the choice is visible and only the active one is
    // coloured.
    QPushButton*             m_gmButton = nullptr;
    QPushButton*             m_mt32Button = nullptr;
    gybokamidi::Target       m_target = gybokamidi::Target::Gm;

    bool                     m_building = false;
    bool                     m_saved = false;
};
