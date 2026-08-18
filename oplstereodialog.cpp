#include "oplstereodialog.h"
#include "uistrings.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QListWidgetItem>
#include <QApplication>

OplStereoDialog::OplStereoDialog(int currentMode, QWidget* parent)
    : QDialog(parent)
    , m_selectedMode(currentMode)
{
    setupUi();
    
    // Select current mode in list (Mode 1 to 9 mapped to row 0 to 8)
    if (currentMode >= 1 && currentMode <= 10) {
        m_listWidget->setCurrentRow(currentMode - 1);
    }
    
    // Install event filter on list widget to intercept raw number key presses
    m_listWidget->installEventFilter(this);
}

OplStereoDialog::~OplStereoDialog() {
}

void OplStereoDialog::setupUi() {
    setWindowTitle("OPL3 Playback Setup");
    setFixedSize(560, 410);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Apply Application Dark Style Sheet
    setStyleSheet(
        "QDialog {"
        "    background-color: #2b2b2b;"
        "    border: 1px solid #555555;"
        "}"
        "QLabel {"
        "    color: #ffffff;"
        "    font-family: 'Consolas', 'Courier New', monospace;"
        "    font-size: 13px;"
        "}"
        "QListWidget {"
        "    background-color: #1e1e1e;"
        "    border: 1px solid #555555;"
        "    outline: none;"
        "}"
        "QListWidget::item {"
        "    border: none;"
        "    background-color: transparent;"
        "}"
        "QListWidget::item:selected {"
        "    background-color: #0078d4;"
        "    color: #ffffff;"
        "}"
    );

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(15, 15, 15, 15);
    layout->setSpacing(10);

    // Title label
    QLabel* titleLabel = new QLabel(this);
    titleLabel->setText("=== OPL3 PLAYBACK MODE SETUP ===");
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 15px; color: #ffcc00; margin-bottom: 5px;");
    layout->addWidget(titleLabel);

    // List widget
    m_listWidget = new QListWidget(this);
    m_listWidget->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(m_listWidget);

    // Items list
    struct ModeItem {
        int mode;
        QString text;
    };

    // The bracketed patterns here are written from the LISTENER's side - the
    // letter says which speaker that channel comes out of. They are deliberately
    // the mirror of the strings in JJoMeSynth::setOplStereoMode(), because that
    // table's letters are the mirror of the chip's own bits: nukedopl.c takes
    // 0x10 as `cha` and mixes `cha` into the LEFT output, while the table
    // returns 0x20 for 'L'. All three implementations - here, the jukebox and
    // the mt32-pi fork - share that inverted convention, so they agree with each
    // other and only the printed letters were ever wrong. Swapping the bits
    // instead would mirror every existing user's stereo image and would have to
    // land in all three at once; only this display was corrected (2026-08-18).
    QList<ModeItem> items = {
        {1, LSTR("[1] 기본 연주 모드                [MMMMMMMMMMM]", "[1] Basic (Mono) Mode            [MMMMMMMMMMM]")},
        {2, "[2] Stereo/Mono 1            [MMLLRRMMMML]"},
        {3, "[3] Stereo/Mono 2            [RRRLLLMMMML]"},
        {4, "[4] Stereo/Mono 3            [RLRLRLMMMML]"},
        {5, "[5] Stereo/Mono 4            [LRLRLRMMMML]"},
        {6, "[6] Stereo/Mono 5            [RRRRLLLLMML]"},
        {7, "[7] Stereo/Mono 6            [LLLLRRRRMML]"},
        {8, "[8] Stereo/Mono 7            [LLLRRRLLLRL]"},
        {9, "[9] Stereo/Mono 8            [RRLLRRLLRRL]"},
        {10, "[0] OKSORI Format Dual-Chip      [L/R detuned]"}
    };

    for (const auto& it : items) {
        QListWidgetItem* listItem = new QListWidgetItem(m_listWidget);
        listItem->setSizeHint(QSize(0, 26));

        QLabel* label = new QLabel(this);
        // Stylize bracketed numbers in red for DOS feeling
        QString stylizedText = it.text;
        const int shown = (it.mode == 10) ? 0 : it.mode;   // 10 is offered as [0]
        stylizedText.replace(QString("[%1]").arg(shown),
            QString("[<font color='#ff5555'><b>%1</b></font>]").arg(shown));
        label->setText(stylizedText);
        label->setStyleSheet("background-color: transparent; color: #ffffff; padding-left: 5px;");

        m_listWidget->addItem(listItem);
        m_listWidget->setItemWidget(listItem, label);
    }

    // Help/Guide label
    QLabel* guideLabel = new QLabel(this);
    guideLabel->setText("[1-9,0] Select Mode  [Enter] Confirm  [Esc] Cancel");
    guideLabel->setAlignment(Qt::AlignCenter);
    guideLabel->setStyleSheet("color: #0078d4; font-weight: bold; margin-top: 5px;");
    layout->addWidget(guideLabel);

    // Double click to accept
    connect(m_listWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        selectModeAndAccept(m_listWidget->row(item) + 1);
    });

    // Enter to accept
    connect(m_listWidget, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        selectModeAndAccept(m_listWidget->row(item) + 1);
    });
}

void OplStereoDialog::selectModeAndAccept(int mode) {
    // 10 is the Oksori layout, offered as [0]. This gate said 9 and silently
    // dropped it - every other range in the path had been widened but this one.
    if (mode >= 1 && mode <= 10) {
        m_selectedMode = mode;
        accept();
    }
}

void OplStereoDialog::keyPressEvent(QKeyEvent* event) {
    int key = event->key();
    // In case the event filter doesn't catch it
    if (key >= Qt::Key_1 && key <= Qt::Key_9) {
        selectModeAndAccept(key - Qt::Key_1 + 1);
        return;
    }
    if (key == Qt::Key_0) { selectModeAndAccept(10); return; }
    QDialog::keyPressEvent(event);
}

// Event filter to handle number key presses even when QListWidget has focus
bool OplStereoDialog::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_listWidget && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        int key = keyEvent->key();
        if (key >= Qt::Key_1 && key <= Qt::Key_9) {
            selectModeAndAccept(key - Qt::Key_1 + 1);
            return true;
        }
        if (key == Qt::Key_0) { selectModeAndAccept(10); return true; }
    }
    return QDialog::eventFilter(obj, event);
}
