// Does the MT-32 panel's ROM selector actually open when clicked?
//
// It did not, and the reason was not obvious from reading it: the box is an
// EDITABLE QComboBox - made so because that is the only reliable way to centre
// its closed-state text - and an editable combo opens its popup from the arrow
// alone, never from a click on the body. With the arrow also hidden by the
// stylesheet, the control looked like a label and did nothing (2026-08-21).
//
// A bug that comes from a widget's documented-but-surprising behaviour is worth
// a test, because the next person to touch the styling will not know either.
// This builds the real window, sends a real mouse press to the real line edit,
// and asks the real combo whether its view came up.
//
// Deliberately NOT part of the player: separate main(), -DBUILD_TESTS=ON only.
//
//   mt32ui [--roms DIR]
//
// A window appears for a moment; there is no way to test a popup without one.
#include <QApplication>
#include <QAbstractItemView>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <iostream>

#include "mt32display.h"
#include "mt32synth.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const QString& what, const QString& detail = QString())
{
    ++g_checks;
    std::cout << (ok ? "ok    " : "FAIL  ") << qPrintable(what);
    if (!detail.isEmpty()) std::cout << "   " << qPrintable(detail);
    std::cout << std::endl;
    if (!ok) ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("mt32ui");

    QCommandLineParser parser;
    parser.setApplicationDescription("Click the MT-32 panel's ROM selector and see if it opens.");
    parser.addHelpOption();
    QCommandLineOption romsOpt("roms", "Folder holding MT-32 / CM-32L ROMs.", "dir");
    parser.addOption(romsOpt);
    parser.process(app);

    const QString romsFrom = parser.value(romsOpt);
    if (!romsFrom.isEmpty()) {
        const QString into = Mt32Synth::InstallDir();
        QDir().mkpath(into);
        for (const QFileInfo& fi : QDir(romsFrom).entryInfoList(QDir::Files)) {
            const QString dst = QDir(into).filePath(fi.fileName());
            if (!QFileInfo::exists(dst)) QFile::copy(fi.absoluteFilePath(), dst);
        }
    }

    const QVector<Mt32Synth::RomSet> sets = Mt32Synth::ScanRomSets();
    if (sets.isEmpty()) {
        std::cout << "no ROMs - pass --roms with a folder of them" << std::endl;
        return 2;
    }
    std::cout << sets.size() << " ROM set(s) found" << std::endl;

    Mt32Synth synth;
    if (!synth.Open()) {
        std::cout << "could not open: " << qPrintable(synth.ErrorString()) << std::endl;
        return 2;
    }

    // A popup is a real top-level window, and it is not mapped until the event
    // loop has run for a bit. One processEvents() is not enough - the first
    // version of this test drew its conclusions from that and reported the code
    // broken when the probe was.
    auto pump = [&](int ms = 300) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };

    Mt32Display panel(&synth);
    panel.show();
    panel.activateWindow();
    pump();

    QComboBox* combo = panel.findChild<QComboBox*>();
    check(combo != nullptr, "the panel has a ROM selector");
    if (!combo) return 1;

    check(combo->count() == sets.size(),
          "it lists every ROM set found",
          QString("%1 in the box, %2 on disk").arg(combo->count()).arg(sets.size()));

    check(combo->isEnabled(), "it is enabled");

    QLineEdit* le = combo->lineEdit();
    check(le != nullptr, "its text is centred through a line edit");
    check(le && le->alignment().testFlag(Qt::AlignHCenter), "and that line edit is centred");
    check(le && le->isReadOnly(), "and read-only, so nothing can be typed into it");

    // "Is the list up" needs establishing before it can be trusted as a verdict
    // on the click - a probe that never says yes proves nothing about the code
    // it is pointed at. So: open it directly first.
    auto popupOpen = [&] {
        return QApplication::activePopupWidget() != nullptr
               || (combo->view() && combo->view()->window()
                   && combo->view()->window()->isVisible());
    };

    combo->showPopup();
    pump(200);
    check(popupOpen(), "showPopup() opens the list (the probe itself works)");
    combo->hidePopup();
    pump(200);
    check(!popupOpen(), "and hidePopup() closes it again");

    // The actual regression: a press on the body has to bring the list up.
    if (le) {
        const QPoint at = le->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, at, le->mapToGlobal(at),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(le, &press);
        pump(200);

        const bool open = popupOpen();
        check(open, "clicking the box opens the list");
        if (open) { combo->hidePopup(); app.processEvents(); }
    }

    // And choosing an entry has to change the machine.
    if (combo->count() > 1) {
        const QString before = synth.CurrentRomId();
        const int other = (combo->currentIndex() + 1) % combo->count();
        combo->setCurrentIndex(other);
        pump(200);
        check(synth.CurrentRomId() != before,
              "choosing another set loads it",
              before + " -> " + synth.CurrentRomId());
    } else {
        std::cout << "skip  only one ROM set present, cannot test switching" << std::endl;
    }

    panel.hide();
    synth.Close();

    std::cout << std::endl << "--- summary ---" << std::endl
              << "  " << g_checks << " checks, " << g_failed << " failed" << std::endl;
    return g_failed > 0 ? 1 : 0;
}
