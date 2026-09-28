// Converts a Ballade .SNG to a Standard MIDI File, which is the only way to
// score the reader against SNG2S over a folder of samples without a GUI.
#include "../sngmidi.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { printf("usage: sngdump <song.sng> [out.mid]\n"); return 2; }

    const QString in = QString::fromLocal8Bit(argv[1]);
    if (!sngmidi::isSngFile(in)) { printf("not a Ballade .SNG\n"); return 1; }

    printf("title: %s\n", sngmidi::extractTitle(in).toUtf8().constData());
    const QByteArray mid = sngmidi::toMidi(in);
    if (mid.isEmpty()) { printf("conversion failed\n"); return 1; }

    const QString out = (argc >= 3) ? QString::fromLocal8Bit(argv[2])
                                    : QFileInfo(in).completeBaseName() + ".mid";
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly)) { printf("cannot write %s\n", out.toLocal8Bit().constData()); return 1; }
    f.write(mid);
    printf("wrote %s (%lld bytes)\n", out.toLocal8Bit().constData(), (long long)mid.size());
    return 0;
}
