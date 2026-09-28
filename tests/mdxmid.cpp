#include "mdxmidi.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) { printf("usage: mdxmid <in.mdx> <out.mid> [seconds]\n"); return 2; }
    const int secs = (argc > 3) ? atoi(argv[3]) : 300;
    const QString in = QString::fromLocal8Bit(argv[1]);
    printf("hasMidiTracks: %s\n", mdxmidi::hasMidiTracks(in) ? "yes" : "no");
    QByteArray smf = mdxmidi::toMidi(in);
    printf("SMF bytes: %d\n", smf.size());
    if (smf.isEmpty()) return 1;
    QFile f(QString::fromLocal8Bit(argv[2]));
    if (!f.open(QIODevice::WriteOnly)) { printf("cannot write\n"); return 1; }
    f.write(smf); f.close();
    printf("written %s\n", argv[2]);
    return 0;
}
