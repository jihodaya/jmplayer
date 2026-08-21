#include "patchdialog.h"

#include "convert/gmmap.h"
#include "mt32map.h"
#include "uistrings.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Column { ColName = 0, ColNotes, ColOrigin, ColType, ColTarget, ColRevert, ColCount };

QColor originColour(gybokamidi::Origin o)
{
    switch (o) {
        case gybokamidi::Origin::SongFile:  return QColor("#4ec9b0");  // the song's own
        case gybokamidi::Origin::NameMatch: return QColor("#cccccc");
        case gybokamidi::Origin::Unmatched: return QColor("#ffcc00");  // needs an ear
        case gybokamidi::Origin::User:      return QColor("#0090ff");
    }
    return QColor("#cccccc");
}

}  // namespace

PatchDialog::PatchDialog(const QString& songPath,
                         const QVector<gybokamidi::Row>& plan, QWidget* parent)
    : QDialog(parent), m_songPath(songPath), m_plan(plan)
{
    setWindowTitle(LSTR("악기 변경", "Change Instruments"));
    resize(760, 520);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    setStyleSheet(
        "QDialog { background-color: #2b2b2b; border: 1px solid #555555; }"
        "QLabel { color: #ffffff; font-size: 13px; }"
        "QCheckBox { color: #ffffff; font-size: 13px; }"
        "QTableWidget { background-color: #1e1e1e; color: #e0e0e0;"
        "  gridline-color: #3a3a3a; border: 1px solid #555555; outline: none; }"
        "QHeaderView::section { background-color: #333333; color: #ffffff;"
        "  border: 0px; border-right: 1px solid #444444; padding: 4px; }"
        "QTableWidget::item:selected { background-color: #0078d4; color: #ffffff; }"
        "QComboBox { background-color: #2d2d34; color: #ffffff;"
        "  border: 1px solid #555555; padding: 2px 6px; }"
        "QComboBox QAbstractItemView { background-color: #1e1e1e; color: #e0e0e0;"
        "  selection-background-color: #0078d4; }"
        "QPushButton { color: #ffffff; background-color: #2d2d34;"
        "  border: 1px solid #555555; border-radius: 4px; padding: 4px 14px; }"
        "QPushButton:hover { background-color: #3b3b45; }"
    );

    // Instruments that carry the song come first: a slot playing three thousand
    // notes is worth more attention than one playing twelve.
    std::sort(m_plan.begin(), m_plan.end(),
              [](const gybokamidi::Row& a, const gybokamidi::Row& b) {
                  return a.notes > b.notes;
              });

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    m_onlyUnmatched = new QCheckBox(
        LSTR("인식하지 못한 것만 보기", "Show only what was not recognised"), this);
    connect(m_onlyUnmatched, &QCheckBox::toggled,
            this, &PatchDialog::onlyUnmatchedToggled);

    m_revertAll = new QPushButton(LSTR("모두 되돌리기", "Revert all"), this);
    m_revertAll->setEnabled(false);
    connect(m_revertAll, &QPushButton::clicked, this, &PatchDialog::revertAll);

    // Which module the list is for. Read before the table is built, since every
    // instrument combo depends on it.
    m_target = gybokamidi::targetModule(songPath);

    const QString targetTip =
        LSTR(u8"악기 목록을 GM과 MT-32 중 어느 쪽으로 보여줄지 고릅니다.\n"
             u8"곡 파일이 갖고 있던 음색 번호는 원래 MT-32 번호입니다.",
             u8"Whether the instrument lists are General MIDI or MT-32.\n"
             u8"The tone numbers these song files carry are MT-32 numbers.");

    m_gmButton = new QPushButton(QStringLiteral("GM"), this);
    m_mt32Button = new QPushButton(QStringLiteral("MT-32"), this);
    for (QPushButton* b : { m_gmButton, m_mt32Button }) {
        b->setCheckable(true);
        b->setAutoExclusive(false);   // set together in updateTargetButton()
        b->setToolTip(targetTip);
        b->setFocusPolicy(Qt::NoFocus);
    }
    connect(m_gmButton, &QPushButton::clicked, this,
            [this] { setTarget(gybokamidi::Target::Gm); });
    connect(m_mt32Button, &QPushButton::clicked, this,
            [this] { setTarget(gybokamidi::Target::Mt32); });

    QHBoxLayout* topRow = new QHBoxLayout();
    topRow->addWidget(m_onlyUnmatched);
    topRow->addStretch();
    topRow->addWidget(m_gmButton);
    topRow->addWidget(m_mt32Button);
    topRow->addSpacing(12);
    topRow->addWidget(m_revertAll);
    layout->addLayout(topRow);

    updateTargetButton();

    m_table = new QTableWidget(this);
    layout->addWidget(m_table, 1);
    buildTable();

    m_hint = new QLabel(
        LSTR(u8"바꾸면 재생 중인 소리에 바로 반영됩니다. "
             u8"저장을 누르기 전에는 파일을 만들지 않습니다.",
             u8"Changes are heard immediately while the song plays. "
             u8"Nothing is written to disk until you press Save."), this);
    m_hint->setStyleSheet("color: #9a9a9a; font-size: 12px;");
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();

    m_saveButton = new QPushButton(LSTR("저장", "Save"), this);
    connect(m_saveButton, &QPushButton::clicked, this, &PatchDialog::save);
    buttons->addWidget(m_saveButton);

    QPushButton* close = new QPushButton(LSTR("닫기", "Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);

    layout->addLayout(buttons);
}

// Wheel over a combo used to change its value - so scrolling the list past a
// row silently reassigned that instrument. Combos here are set NoFocus and
// their wheel events are dropped; the table scrolls instead, which is what the
// gesture was meant to do.
bool PatchDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel && qobject_cast<QComboBox*>(obj)) {
        if (m_table && m_table->viewport())
            QCoreApplication::sendEvent(m_table->viewport(), event);
        return true;
    }
    return QDialog::eventFilter(obj, event);
}

void PatchDialog::save()
{
    if (gybokamidi::saveSidecar(m_songPath, m_plan)) {
        m_saved = true;
        m_hint->setText(LSTR(u8"저장했습니다: ", u8"Saved to: ") +
                        gybokamidi::sidecarPath(m_songPath));
        m_hint->setStyleSheet("color: #4ec9b0; font-size: 12px;");
    } else {
        m_hint->setText(LSTR(u8"저장하지 못했습니다. 쓰기 권한을 확인해 주세요.",
                             u8"Could not save. Check write permission."));
        m_hint->setStyleSheet("color: #ff8080; font-size: 12px;");
    }
}

void PatchDialog::buildTable()
{
    m_building = true;

    m_table->setColumnCount(ColCount);
    m_table->setRowCount(m_plan.size());
    m_table->setHorizontalHeaderLabels({
        LSTR("OPL 악기", "OPL instrument"),
        LSTR("음표", "Notes"),
        LSTR("기본값 출처", "Default from"),
        LSTR("종류", "Type"),
        LSTR("바꿀 악기", "Play as"),
        QString(),
    });
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(30);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QHeaderView* h = m_table->horizontalHeader();
    h->setSectionResizeMode(ColName,   QHeaderView::ResizeToContents);
    h->setSectionResizeMode(ColNotes,  QHeaderView::ResizeToContents);
    h->setSectionResizeMode(ColOrigin, QHeaderView::ResizeToContents);
    h->setSectionResizeMode(ColType,   QHeaderView::ResizeToContents);
    h->setSectionResizeMode(ColTarget, QHeaderView::Stretch);
    h->setSectionResizeMode(ColRevert, QHeaderView::ResizeToContents);

    for (int i = 0; i < m_plan.size(); ++i) {
        const gybokamidi::Row& r = m_plan[i];

        m_table->setItem(i, ColName, new QTableWidgetItem(r.oplName));
        QTableWidgetItem* n = new QTableWidgetItem(QString::number(r.notes));
        n->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(i, ColNotes, n);

        m_table->setItem(i, ColOrigin, new QTableWidgetItem(QString()));

        QComboBox* type = new QComboBox(m_table);
        type->addItem(LSTR("멜로디", "Melodic"));
        type->addItem(LSTR("타악기", "Percussion"));
        type->setCurrentIndex(r.drum ? 1 : 0);
        type->setFocusPolicy(Qt::StrongFocus);   // no wheel unless clicked into
        type->installEventFilter(this);
        connect(type, &QComboBox::currentIndexChanged, this,
                [this, i](int idx) { onDrumToggled(i, idx == 1); });
        m_table->setCellWidget(i, ColType, type);

        QComboBox* target = new QComboBox(m_table);
        target->setFocusPolicy(Qt::StrongFocus);
        target->installEventFilter(this);
        m_table->setCellWidget(i, ColTarget, target);
        connect(target, &QComboBox::currentIndexChanged, this,
                [this, i](int idx) { onTargetChanged(i, idx); });
        fillTargetCombo(i);

        QPushButton* rev = new QPushButton(LSTR("되돌리기", "Revert"), m_table);
        connect(rev, &QPushButton::clicked, this, [this, i] { revertRow(i); });
        m_table->setCellWidget(i, ColRevert, rev);

        refreshRow(i);
    }

    m_building = false;
    applyFilter();
}

// Melodic rows choose from the 128 GM programs, percussion rows from the drum
// notes - two different lists behind one column, swapped when Type changes.
void PatchDialog::fillTargetCombo(int row)
{
    QComboBox* c = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColTarget));
    if (!c) return;

    const bool wasBuilding = m_building;
    m_building = true;

    c->clear();
    const gybokamidi::Row& r = m_plan[row];

    // MT-32 mode lists that machine's 128 tones instead of the GM set. They are
    // different instruments, not different names for the same ones - `Fantasy`,
    // `Doctor Solo` and `Jungle Tune` exist nowhere else - so the list has to
    // change with the target or the numbers mean nothing.
    //
    // Percussion still uses the GM drum list. The MT-32's rhythm part has its
    // own note layout, but it is close enough to GM at the notes these songs
    // actually use, and inventing a second drum table from nothing would be a
    // guess where this one is measured.
    if (m_target == gybokamidi::Target::Mt32 && !r.drum) {
        for (int t = 1; t <= 128; ++t) {
            c->addItem(QString("%1  %2")
                           .arg(t, 3)
                           .arg(QString::fromLatin1(mt32map::toneName(t))),
                       t);
        }
        int idx = c->findData(r.mt32Program);
        c->setCurrentIndex(idx >= 0 ? idx : 0);
        m_building = wasBuilding;
        return;
    }

    if (r.drum) {
        for (int note : jmpconv::gmDrumNotes()) {
            c->addItem(QString("%1  (%2)")
                           .arg(QString::fromLatin1(jmpconv::gmDrumName(note)))
                           .arg(note),
                       note);
        }
        int idx = c->findData(r.drumNote);
        c->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
        for (int p = 0; p < 128; ++p) {
            c->addItem(QString("%1  %2")
                           .arg(p + 1, 3)
                           .arg(QString::fromLatin1(jmpconv::gmProgramName(p))),
                       p);
        }
        int idx = c->findData(r.program);
        c->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    m_building = wasBuilding;
}

void PatchDialog::refreshRow(int row)
{
    const gybokamidi::Row& r = m_plan[row];

    QString origin = gybokamidi::originLabel(r.origin);
    if (r.origin == gybokamidi::Origin::SongFile && r.mt32Tone > 0) {
        // Say which MT-32 tone it was. The number is what the DOS screen showed,
        // so a listener can compare the two directly.
        origin += QString(" · MT-32 %1 %2")
                      .arg(r.mt32Tone)
                      .arg(QString::fromLatin1(mt32map::toneName(r.mt32Tone)));
        if (r.approximate) origin += LSTR("  (근사)", "  (approx.)");
    }
    if (r.edited()) origin = gybokamidi::originLabel(gybokamidi::Origin::User);

    QTableWidgetItem* item = m_table->item(row, ColOrigin);
    if (item) {
        item->setText(origin);
        item->setForeground(originColour(r.edited() ? gybokamidi::Origin::User
                                                    : r.origin));
    }

    // A changed row is marked across its whole width, not just in the origin
    // column: with thirty instruments on screen the eye needs to find what it
    // touched without reading every line. Both the text colour and a tinted
    // background change, so it stays visible while the row is selected.
    const bool edited = r.edited();
    const QBrush fg(edited ? originColour(gybokamidi::Origin::User)
                           : QColor("#e0e0e0"));
    const QBrush bg(edited ? QColor("#152a3d") : QColor(Qt::transparent));
    for (int c = ColName; c <= ColOrigin; ++c) {
        if (QTableWidgetItem* cell = m_table->item(row, c)) {
            if (c != ColOrigin) cell->setForeground(fg);
            cell->setBackground(bg);
            QFont f = cell->font();
            f.setBold(edited);
            cell->setFont(f);
        }
    }

    if (m_revertAll) {
        int n = 0;
        for (const gybokamidi::Row& x : m_plan) if (x.edited()) ++n;
        m_revertAll->setEnabled(n > 0);
    }

    int unmatched = 0, fromFile = 0, changed = 0;
    for (const gybokamidi::Row& x : m_plan) {
        if (x.edited()) ++changed;
        else if (x.origin == gybokamidi::Origin::Unmatched) ++unmatched;
        else if (x.origin == gybokamidi::Origin::SongFile) ++fromFile;
    }
    m_summary->setText(
        LSTR(u8"악기 %1개 · 곡에 저장된 배정 %2개 · 인식 못 함 %3개 · 바꾼 것 %4개",
             u8"%1 instruments · %2 from the song · %3 unrecognised · %4 changed")
            .arg(m_plan.size()).arg(fromFile).arg(unmatched).arg(changed));
}

void PatchDialog::onDrumToggled(int row, bool drum)
{
    if (m_building || row < 0 || row >= m_plan.size()) return;
    m_plan[row].drum = drum;
    fillTargetCombo(row);
    refreshRow(row);
    emit assignmentChanged(m_plan[row].slot, m_plan[row]);
}

void PatchDialog::onTargetChanged(int row, int index)
{
    if (m_building || row < 0 || row >= m_plan.size()) return;
    QComboBox* c = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColTarget));
    if (!c || index < 0) return;

    const int value = c->itemData(index).toInt();
    if (m_plan[row].drum)
        m_plan[row].drumNote = value;
    else if (m_target == gybokamidi::Target::Mt32)
        m_plan[row].mt32Program = value;      // 1..128
    else
        m_plan[row].program = value;

    refreshRow(row);
    emit assignmentChanged(m_plan[row].slot, m_plan[row]);
}

void PatchDialog::revertRow(int row)
{
    if (row < 0 || row >= m_plan.size()) return;
    gybokamidi::Row& r = m_plan[row];
    r.drum     = r.baseDrum;
    r.program  = r.baseProgram;
    r.bankMsb  = r.baseBankMsb;
    r.drumNote = r.baseDrumNote;
    r.mt32Program = r.baseMt32Program;
    r.origin   = r.baseOrigin;

    QComboBox* type = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColType));
    if (type) {
        m_building = true;
        type->setCurrentIndex(r.drum ? 1 : 0);
        m_building = false;
    }
    fillTargetCombo(row);
    refreshRow(row);
    emit assignmentChanged(r.slot, r);
}

// Every row back to the default it opened with, and the sound with it - the
// same program changes go out as if each had been reverted by hand, so an
// experiment can be abandoned without restarting the song.
void PatchDialog::revertAll()
{
    for (int i = 0; i < m_plan.size(); ++i)
        if (m_plan[i].edited()) revertRow(i);
}

void PatchDialog::onlyUnmatchedToggled(bool) { applyFilter(); }

void PatchDialog::setTarget(gybokamidi::Target target)
{
    if (target == m_target) {
        updateTargetButton();   // a click on the one already active: just redraw
        return;
    }

    m_target = target;
    gybokamidi::setTargetModule(m_songPath, m_target);
    updateTargetButton();

    // Every melodic list is now the wrong set, and every melodic row is now
    // playing a different instrument. Rebuild the lists, then re-announce each
    // row so the sound follows the switch the way a single edit does.
    m_building = true;
    for (int i = 0; i < m_plan.size(); ++i)
        fillTargetCombo(i);
    m_building = false;

    for (int i = 0; i < m_plan.size(); ++i) {
        refreshRow(i);
        emit assignmentChanged(m_plan[i].slot, m_plan[i]);
    }
}

void PatchDialog::updateTargetButton()
{
    if (!m_gmButton || !m_mt32Button) return;

    const bool mt32 = (m_target == gybokamidi::Target::Mt32);
    m_gmButton->setChecked(!mt32);
    m_mt32Button->setChecked(mt32);

    // Only the active one is coloured, in jmp's accent. A checked QPushButton
    // is drawn as merely "pressed" by the default style, which next to an
    // unchecked one of the same size is easy to miss.
    const char* kActive =
        "QPushButton { background-color: #0078d4; color: white;"
        " border: 1px solid #0078d4; border-radius: 3px; padding: 4px 14px; }";
    const char* kIdle =
        "QPushButton { background-color: #3a3a3a; color: #b0b0b0;"
        " border: 1px solid #555555; border-radius: 3px; padding: 4px 14px; }"
        "QPushButton:hover { background-color: #4a4a4a; color: white; }";

    m_gmButton->setStyleSheet(QString::fromLatin1(mt32 ? kIdle : kActive));
    m_mt32Button->setStyleSheet(QString::fromLatin1(mt32 ? kActive : kIdle));
}

void PatchDialog::applyFilter()
{
    const bool only = m_onlyUnmatched && m_onlyUnmatched->isChecked();
    for (int i = 0; i < m_plan.size(); ++i) {
        const bool hide = only && !(m_plan[i].origin == gybokamidi::Origin::Unmatched
                                    && !m_plan[i].edited());
        m_table->setRowHidden(i, hide);
    }
}
