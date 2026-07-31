//
// midiresetdialog.cpp
//
#include "midiresetdialog.h"
#include "midireset.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "settingsmanager.h"
#include "uistrings.h"

MidiResetDialog::MidiResetDialog(MidiReset* pReset, QWidget* parent)
    : QDialog(parent), m_pReset(pReset) {
    setupUi();
}

void MidiResetDialog::setupUi() {
    setWindowTitle(LSTR(u8"음원 리셋 설정", u8"Sound Module Reset"));
    setModal(true);

    SettingsManager& s = SettingsManager::instance();
    const bool enabled = s.value("Midi/ResetEnabled", false).toBool();
    const unsigned flags = s.value("Midi/ResetFlags",
                                   (unsigned)MidiReset::DefaultGMGS).toUInt();
    const unsigned delayMs = s.value("Midi/ResetDelayMs",
        MidiReset::DefaultDelayMs).toUInt();

    auto* layout = new QVBoxLayout(this);

    m_enable = new QCheckBox(
        LSTR(u8"곡을 새로 재생할 때 음원을 리셋", u8"Reset the sound module on each new song"), this);
    m_enable->setChecked(enabled);
    layout->addWidget(m_enable);

    auto* info = new QLabel(
        LSTR(u8"외부 MIDI 장치로 재생할 때만 전송됩니다. 이전 곡이 남긴\n"
             u8"음색·리버브·튜닝 등을 지워 새 곡이 깨끗하게 재생됩니다.",
             u8"Sent only when playing to an external MIDI device. Clears the\n"
             u8"previous song's instruments/reverb/tuning so the next plays clean."),
        this);
    info->setStyleSheet("color:#999; font-size:11px;");
    layout->addWidget(info);

    // Which reset messages
    auto* box = new QGroupBox(LSTR(u8"전송할 리셋", u8"Resets to send"), this);
    auto* boxLayout = new QVBoxLayout(box);
    m_gm   = new QCheckBox(LSTR(u8"GM System On", u8"GM System On"), box);
    m_gs   = new QCheckBox(LSTR(u8"GS Reset (Roland)", u8"GS Reset (Roland)"), box);
    m_xg   = new QCheckBox(LSTR(u8"XG System On (Yamaha)", u8"XG System On (Yamaha)"), box);
    m_mt32 = new QCheckBox(LSTR(u8"MT-32 Reset", u8"MT-32 Reset"), box);
    m_gm->setChecked(flags & MidiReset::GM);
    m_gs->setChecked(flags & MidiReset::GS);
    m_xg->setChecked(flags & MidiReset::XG);
    m_mt32->setChecked(flags & MidiReset::MT32);
    boxLayout->addWidget(m_gm);
    boxLayout->addWidget(m_gs);
    boxLayout->addWidget(m_xg);
    boxLayout->addWidget(m_mt32);
    auto* hint = new QLabel(
        LSTR(u8"권장: GM + GS (이 프로젝트의 mt32-pi에 맞음).\n"
             u8"MT-32는 다른 규격이라 단독 선택 시에만 전송됩니다.",
             u8"Recommended: GM + GS (suits this project's mt32-pi).\n"
             u8"MT-32 is a different standard - sent only if selected alone."),
        box);
    hint->setStyleSheet("color:#999; font-size:11px;");
    boxLayout->addWidget(hint);
    layout->addWidget(box);

    // Settle delay - applied between resets AND after the last one, so the
    // module has time to act on the reset before the song's first notes.
    auto* delayRow = new QHBoxLayout();
    delayRow->addWidget(new QLabel(
        LSTR(u8"리셋 후 대기", u8"Wait after reset"), this));
    m_delay = new QSpinBox(this);
    m_delay->setRange(0, 500);
    m_delay->setSuffix(" ms");
    m_delay->setValue((int)delayMs);
    m_delay->setToolTip(
        LSTR(u8"음원이 리셋을 처리할 시간입니다. 각 리셋 메시지마다 이만큼 쉬므로,\n"
             u8"곡 시작이 (대기 x 리셋 개수)만큼 늦어집니다. 재생 박자에는 영향이 없습니다.\n"
             u8"SC-55 계열 하드웨어/에뮬레이터는 50~100ms 권장, 소프트웨어 음원은 0으로 두세요.",
             u8"Time for the module to act on the reset. Applied per reset message,\n"
             u8"so the song starts (delay x number of resets) later. Playback timing is unaffected.\n"
             u8"SC-55 style hardware/emulators: 50-100 ms. Leave 0 for software synths."));
    delayRow->addWidget(m_delay);
    delayRow->addStretch();
    layout->addLayout(delayRow);

    // Same guidance as the spin box's tooltip, but visible without hovering -
    // this is the setting users are most likely to need to change, and the
    // right value depends entirely on what they are playing into.
    // No background colours here: the dialog has to stay readable under the
    // dark theme (see the note in jmp/CLAUDE.md about the help dialog).
    auto* delayHint = new QLabel(
        LSTR(u8"<table cellspacing='0' cellpadding='2'>"
             u8"<tr><td>SC-55 계열 하드웨어 / Nuked-SC55</td><td><b>50~100ms</b></td></tr>"
             u8"<tr><td>소프트웨어 신스 (mt32-pi 등)</td><td><b>0</b></td></tr>"
             u8"</table>"
             u8"리셋 메시지마다 이만큼 쉬므로 곡 시작이 (대기 x 리셋 개수)만큼<br>"
             u8"늦어집니다. 재생 박자에는 영향이 없습니다.",

             u8"<table cellspacing='0' cellpadding='2'>"
             u8"<tr><td>SC-55 style hardware / Nuked-SC55</td><td><b>50-100 ms</b></td></tr>"
             u8"<tr><td>Software synths (mt32-pi etc.)</td><td><b>0</b></td></tr>"
             u8"</table>"
             u8"Applied per reset message, so the song starts (delay x number of<br>"
             u8"resets) later. Playback timing is unaffected."),
        this);
    delayHint->setTextFormat(Qt::RichText);
    delayHint->setStyleSheet("color:#999; font-size:11px;");
    layout->addWidget(delayHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &MidiResetDialog::applyAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_enable, &QCheckBox::toggled, this, &MidiResetDialog::syncEnabledState);
    syncEnabledState();
}

void MidiResetDialog::syncEnabledState() {
    const bool on = m_enable->isChecked();
    m_gm->setEnabled(on);
    m_gs->setEnabled(on);
    m_xg->setEnabled(on);
    m_mt32->setEnabled(on);
    m_delay->setEnabled(on);
}

void MidiResetDialog::applyAndAccept() {
    unsigned flags = MidiReset::None;
    if (m_gm->isChecked())   flags |= MidiReset::GM;
    if (m_gs->isChecked())   flags |= MidiReset::GS;
    if (m_xg->isChecked())   flags |= MidiReset::XG;
    if (m_mt32->isChecked()) flags |= MidiReset::MT32;

    const bool enabled = m_enable->isChecked();
    const unsigned delayMs = (unsigned)m_delay->value();

    SettingsManager& s = SettingsManager::instance();
    s.setValue("Midi/ResetEnabled", enabled);
    s.setValue("Midi/ResetFlags", flags);
    s.setValue("Midi/ResetDelayMs", delayMs);

    if (m_pReset) {
        m_pReset->SetEnabled(enabled);
        m_pReset->SetFlags(flags);
        m_pReset->SetInterMessageDelayMs(delayMs);
    }

    accept();
}
