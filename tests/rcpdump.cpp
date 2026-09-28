// Convert a Recomposer file to a Standard MIDI File and write it out, so the
// result can be compared against a reference .MID of the same song.
//
// That comparison is the whole reason this exists. The RCP work was reported as
// "it plays, but it sounds different from the .MID" - which is not something to
// chase by ear when two of the songs here (BAMBOO, 3DAYS) ship with a reference
// SMF beside them. Convert, then diff the two event streams.
//
//   rcpdump <file.rcp> [out.mid]
//
// Prints the title and a one-line summary; writes the SMF next to the source as
// <name>.rcp.mid unless an output path is given.
#include "../rcpfilehandler.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::cout << "usage: rcpdump <file.rcp|.g36|.r36|.sng> [out.mid]" << std::endl;
        return 2;
    }

    const QString in = QString::fromLocal8Bit(argv[1]);
    if (!QFile::exists(in)) {
        std::cout << "no such file: " << qPrintable(in) << std::endl;
        return 2;
    }

    const QString title = RcpFileHandler::extractTitle(in);
    const QByteArray midi = RcpFileHandler::extractMidiData(in);
    if (midi.isEmpty()) {
        std::cout << "conversion produced nothing" << std::endl;
        return 1;
    }

    const QString out = (argc >= 3) ? QString::fromLocal8Bit(argv[2])
                                    : in + ".mid";
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly)) {
        std::cout << "cannot write " << qPrintable(out) << std::endl;
        return 1;
    }
    f.write(midi);
    f.close();

    std::cout << qPrintable(QFileInfo(in).fileName())
              << "  ->  " << qPrintable(QFileInfo(out).fileName())
              << "   " << midi.size() << " bytes"
              << "   title: " << qPrintable(title.isEmpty() ? QString("(none)") : title)
              << std::endl;
    return 0;
}
