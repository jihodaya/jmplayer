#include "mt32display.h"
#include "mt32synth.h"
#include "settingsmanager.h"
#include "uistrings.h"
#include "windowplacement.h"

#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// The hardware is a 20-character single-line LCD, so that is what gets drawn -
// twenty cells of a fixed width, not a proportional string. Anything shorter
// than twenty characters is padded, which keeps the panel from appearing to
// change width as messages come and go.
constexpr int kColumns = 20;

// Cell metrics. Chosen so the whole panel lands near the size of the real
// thing next to jmp rather than to match any particular font.
constexpr int kCellW   = 15;
constexpr int kCellH   = 26;
constexpr int kPadX    = 12;
constexpr int kPadY    = 10;
constexpr int kLedW    = 10;

// jmp's own palette, not the hardware's.
//
// The player's readouts - the time display, the track info - are bright text on
// black inside an inset grey border, on a #2b2b2b window. A green-on-dark-green
// LCD looked like a different program bolted on, so the panel uses jmp's
// display colours and only the character grid says "MT-32".
const QColor kPanelDark   (0x1e, 0x1e, 0x1e);   // behind the glass
const QColor kGlassTop    (0x00, 0x00, 0x00);
const QColor kGlassBottom (0x00, 0x00, 0x00);
const QColor kSegOn       (0x00, 0xFF, 0x00);   // #00FF00, as timeDisplayLabel
const QColor kSegOff      (0x0E, 0x24, 0x0E);   // the faint "unlit segment" wash
const QColor kBezel       (0x66, 0x66, 0x66);   // the 2px inset border jmp uses
const QColor kLedOn       (0x00, 0xFF, 0xFF);   // #00FFFF, as trackInfoLabel
const QColor kLedOff      (0x14, 0x30, 0x30);

// Window chrome, matched to mainwindow.cpp.
const char* const kWindowStyle =
    "QWidget { background-color: #2b2b2b; color: white; }"
    "QComboBox {"
    "  background-color: #3a3a3a; color: white;"
    "  border: 1px solid #555555; border-radius: 3px;"
    "  padding: 3px 6px; min-height: 20px;"
    "}"
    "QComboBox:hover { background-color: #4a4a4a; }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    // Styling a combo box removes the native arrow, and without one the box
    // reads as a label rather than as something to click. Drawn from borders
    // so it needs no image resource.
    "QComboBox::down-arrow {"
    "  width: 0; height: 0;"
    "  border-left: 4px solid transparent;"
    "  border-right: 4px solid transparent;"
    "  border-top: 5px solid #cccccc;"
    "  margin-right: 6px;"
    "}"
    "QComboBox::down-arrow:hover { border-top-color: #ffffff; }"
    "QComboBox QAbstractItemView {"
    "  background-color: #2b2b2b; color: white;"
    "  border: 1px solid #555555;"
    "  selection-background-color: #0078d4; selection-color: white;"
    "}";

QFont LcdFont()
{
    // A fixed-pitch face, whatever the system has. The cells are laid out by
    // hand, so the font only has to be monospaced enough to look right inside
    // one - the drawing does not rely on its advance width.
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const char* name : { "Consolas", "DejaVu Sans Mono", "Courier New" }) {
        const QFont candidate(QString::fromLatin1(name));
        if (QFontInfo(candidate).fixedPitch()) { f = candidate; break; }
    }
    f.setPixelSize(18);
    f.setBold(true);
    return f;
}

} // namespace

// ---------------------------------------------------------------- Mt32Lcd

Mt32Lcd::Mt32Lcd(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(sizeHint());
}

QSize Mt32Lcd::sizeHint() const
{
    return QSize(kPadX * 2 + kColumns * kCellW + kLedW + 8,
                 kPadY * 2 + kCellH);
}

void Mt32Lcd::setText(const QString& text)
{
    if (m_text == text)
        return;
    m_text = text;
    update();
}

void Mt32Lcd::setLed(bool on)
{
    if (m_bLed == on)
        return;
    m_bLed = on;
    update();
}

void Mt32Lcd::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full = rect();

    // Black glass in a 2px inset grey border - the same treatment jmp gives its
    // time and track readouts, so the panel belongs to the same program.
    p.fillRect(full, kPanelDark);

    QLinearGradient glass(full.topLeft(), full.bottomLeft());
    glass.setColorAt(0.0, kGlassTop);
    glass.setColorAt(1.0, kGlassBottom);

    QPainterPath glassPath;
    glassPath.addRect(full.adjusted(2, 2, -2, -2));
    p.fillPath(glassPath, glass);

    p.setPen(QPen(kBezel, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(full.adjusted(1, 1, -1, -1));

    // Twenty cells. The unlit wash is drawn for every cell so blanks still look
    // like part of the display.
    QString text = m_text.left(kColumns);
    text = text.leftJustified(kColumns, ' ');

    p.setFont(LcdFont());

    for (int i = 0; i < kColumns; ++i) {
        const QRectF cell(kPadX + i * kCellW, kPadY, kCellW, kCellH);

        p.setPen(Qt::NoPen);
        p.fillRect(cell.adjusted(1, 1, -1, -1), kSegOff);

        const QChar ch = text.at(i);
        if (ch == ' ')
            continue;

        // Character 1 is not a character.
        //
        // The MT-32's display is a DM2011 with four user-programmable glyphs in
        // CGRAM, and #1 is a solid block. In the main screen it replaces a
        // part's number for as long as that part has a note sounding - which is
        // the most useful thing on the panel, since it shows the music moving.
        // munt hands it over as byte 0x01 (Display.cpp, ACTIVE_PART_INDICATOR),
        // and drawing that as text produces the "?" boxes reported 2026-08-21.
        if (ch.unicode() == 0x01) {
            p.setPen(Qt::NoPen);
            p.setBrush(kSegOn);
            p.drawRect(cell.adjusted(2, 3, -2, -3));
            continue;
        }

        p.setPen(kSegOn);
        p.setBrush(Qt::NoBrush);
        p.drawText(cell, Qt::AlignCenter, QString(ch));
    }

    // MIDI MESSAGE lamp, at the right where the hardware has its indicators.
    const QRectF led(full.right() - kPadX - kLedW, full.center().y() - kLedW / 2.0,
                     kLedW, kLedW);
    p.setPen(Qt::NoPen);
    p.setBrush(m_bLed ? kLedOn : kLedOff);
    p.drawEllipse(led);
}

// ------------------------------------------------------------ Mt32Display

Mt32Display::Mt32Display(Mt32Synth* pSynth, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
    , m_pSynth(pSynth)
{
    setWindowTitle(LSTR(u8"MT-32", u8"MT-32"));

    setStyleSheet(QString::fromLatin1(kWindowStyle));

    m_pLcd = new Mt32Lcd(this);

    // The ROM name is shown once, centred inside its own box. It used to be
    // drawn twice - as a label on the left and again as the selector's text -
    // which read as a mistake rather than as information.
    m_pRoms = new QComboBox(this);
    m_pRoms->setFocusPolicy(Qt::NoFocus);
    m_pRoms->setToolTip(LSTR(u8"MT32ROMs 폴더에서 찾은 롬 세트",
                             u8"ROM sets found in the MT32ROMs folder"));

    // A QComboBox draws its closed-state text through the style, which offers no
    // alignment. The reliable way to centre it is to give it a line edit and
    // make that read-only: the caret and focus frame are what usually make this
    // look wrong, and read-only plus no focus policy removes both.
    //
    // The catch, and it is a real one: an EDITABLE combo only opens its popup
    // from the arrow, not from a click anywhere on the box. Together with a
    // stylesheet that had hidden the arrow, that left the selector looking like
    // a label and doing nothing at all - reported 2026-08-21. The event filter
    // below puts the click-anywhere behaviour back.
    m_pRoms->setEditable(true);
    if (QLineEdit* le = m_pRoms->lineEdit()) {
        le->setReadOnly(true);
        le->setAlignment(Qt::AlignCenter);
        le->setFocusPolicy(Qt::NoFocus);
        le->setContextMenuPolicy(Qt::NoContextMenu);
        le->setCursor(Qt::ArrowCursor);   // not a text I-beam: nothing is typed here
        le->setStyleSheet("background: transparent; border: none; color: white;");
        le->installEventFilter(this);
    }

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(10, 10, 10, 10);
    col->setSpacing(8);
    col->addWidget(m_pLcd);
    col->addWidget(m_pRoms);

    setLayout(col);
    setFixedSize(sizeHint());

    refreshRomList();
    connect(m_pRoms, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Mt32Display::onRomChanged);

    // Twenty a second is far more than the hardware ever changes its display,
    // and cheap: it is one string copy under an uncontended lock.
    m_pTimer = new QTimer(this);
    m_pTimer->setInterval(50);
    connect(m_pTimer, &QTimer::timeout, this, &Mt32Display::onPoll);
}

void Mt32Display::refreshRomList()
{
    if (!m_pRoms)
        return;

    const QString keep = m_pSynth ? m_pSynth->CurrentRomId() : QString();

    m_pRoms->blockSignals(true);
    m_pRoms->clear();
    for (const Mt32Synth::RomSet& s : Mt32Synth::ScanRomSets())
        m_pRoms->addItem(s.label, s.id);

    if (!keep.isEmpty()) {
        const int at = m_pRoms->findData(keep);
        if (at >= 0)
            m_pRoms->setCurrentIndex(at);
    }
    m_pRoms->blockSignals(false);

    // Enabled even with one set. Greying it out was meant to say "nothing to
    // choose between", but next to a box that already looked like a label it
    // just read as broken - and it hides the fact that this is where a second
    // ROM set would appear.
    m_pRoms->setEnabled(m_pRoms->count() > 0);
    updateTitle();
}

void Mt32Display::updateTitle()
{
    const QString rom = m_pSynth ? m_pSynth->CurrentRomLabel() : QString();
    setWindowTitle(rom.isEmpty() ? LSTR(u8"MT-32", u8"MT-32")
                                 : QStringLiteral("MT-32 - %1").arg(rom));
}

void Mt32Display::onRomChanged(int index)
{
    if (!m_pSynth || index < 0)
        return;

    const QString id = m_pRoms->itemData(index).toString();
    if (id.isEmpty() || id == m_pSynth->CurrentRomId())
        return;

    // Reopening swaps the whole machine, so anything sounding stops with it.
    // That is the same as power-cycling the real unit and is what the user is
    // asking for by changing the ROM - but a sequencer still running into it
    // while it is torn down and rebuilt is heard as the sound breaking up.
    // The host stops the song around the swap and starts it again afterwards.
    emit romChangeAboutToApply();

    if (m_pSynth->Open(id))
        SettingsManager::instance().setValue("Mt32/RomSet", id);

    updateTitle();

    emit romChangeApplied();
}

void Mt32Display::onPoll()
{
    if (!m_pSynth || !m_pSynth->IsOpen())
        return;

    m_pLcd->setText(m_pSynth->DisplayText());
    m_pLcd->setLed(m_pSynth->MidiLed());
}

void Mt32Display::placeBesideMainWindow()
{
    WindowPlacement::PlaceBesideMainWindow(this);
}

bool Mt32Display::eventFilter(QObject* obj, QEvent* event)
{
    // Click anywhere on the (read-only) box to open the list, the way a
    // non-editable combo behaves. See the constructor for why it is editable.
    if (m_pRoms && obj == m_pRoms->lineEdit()
        && event->type() == QEvent::MouseButtonPress) {
        if (m_pRoms->isEnabled())
            m_pRoms->showPopup();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void Mt32Display::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // The caption is the system's, not the stylesheet's, so it has to be asked
    // for separately - and only once the window has a native handle.
    WindowPlacement::ApplyDarkTitleBar(this);

    refreshRomList();
    if (m_pTimer)
        m_pTimer->start();

    // Only on the first show: after that the user may have moved it, and
    // dragging it back every time the window reappears would be obnoxious.
    if (!m_bPlaced) {
        m_bPlaced = true;
        placeBesideMainWindow();
    }
}

void Mt32Display::hideEvent(QHideEvent* event)
{
    if (m_pTimer)
        m_pTimer->stop();
    QWidget::hideEvent(event);
}
