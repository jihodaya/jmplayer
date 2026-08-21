#include "mt32synth.h"
#include "uistrings.h"

#include <mt32emu/mt32emu.h>
#include <mt32emu/FileStream.h>
#include <mt32emu/SampleRateConverter.h>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace {

// munt's machine identifiers, made readable. The table is short and closed -
// mt32emu ships a fixed list - so a lookup beats parsing.
QString PrettyMachineName(const QString& id)
{
    if (id == "mt32_bluer")  return QStringLiteral("MT-32 (Blue Ridge)");
    if (id.startsWith("cm32ln_")) return QStringLiteral("CM-32LN v") + id.mid(7).replace('_', '.');
    if (id.startsWith("cm32l_"))  return QStringLiteral("CM-32L v")  + id.mid(6).replace('_', '.');
    if (id.startsWith("mt32_"))   return QStringLiteral("MT-32 v")   + id.mid(5).replace('_', '.');
    return id;
}

// The largest thing that can legitimately be a ROM here is the CM-32L PCM at
// 1 MB. Reading every file in the folder is fine, but not every file that
// happens to be in it.
constexpr qint64 kMaxRomBytes = 1024 * 1024;

const char* const kReadmeKo =
    "이 폴더에 MT-32 롬을 넣으세요\n"
    "Put your MT-32 ROMs in this folder\n"
    "================================================================\n"
    "\n"
    "jmp의 MT-32 엔진은 munt(mt32emu)를 씁니다. 에뮬레이터는 함께\n"
    "배포되지만(LGPL) 롬은 롤랜드의 저작물이라 직접 구하셔야 합니다.\n"
    "\n"
    "넣을 것 - 컨트롤 롬 하나와 PCM 롬 하나가 짝이 맞아야 합니다.\n"
    "\n"
    "  MT-32 구형   MT32_CONTROL.ROM + MT32_PCM.ROM\n"
    "  MT-32 신형   같은 이름의 v2.x 롬\n"
    "  CM-32L       CM32L_CONTROL.ROM + CM32L_PCM.ROM\n"
    "\n"
    "파일 이름은 상관없습니다. 내용을 해시로 판별하므로 아무 이름이나\n"
    "두셔도 됩니다. 여러 세트를 같이 넣어두면 화면에서 골라 쓸 수\n"
    "있습니다.\n"
    "\n"
    "반쪽으로 나뉜 덤프(상위/하위 절반)는 아직 지원하지 않습니다.\n"
    "합쳐진 파일을 넣어주세요.\n";

} // namespace

QString Mt32Synth::InstallDir()
{
    return QDir(QApplication::applicationDirPath()).absoluteFilePath("MT32ROMs");
}

void Mt32Synth::EnsureInstallDir()
{
    const QString dir = InstallDir();
    QDir().mkpath(dir);

    const QString note = QDir(dir).absoluteFilePath("읽어보세요.txt");
    if (QFileInfo::exists(note))
        return;

    QFile f(note);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream t(&f);
        t.setEncoding(QStringConverter::Utf8);
        t << QString::fromUtf8(kReadmeKo);
    }
}

QString Mt32Synth::DeviceLabel()
{
    // Bracketed like the other built-in engines, and named for jmp rather than
    // for Roland: Windows may well be showing a port called "MT-32 Synth
    // Emulator" at the same time - munt's own WinMM driver, a separate
    // installation - and the two have to be told apart at a glance.
    return QStringLiteral("[MT32 JMP]");
}

QVector<Mt32Synth::RomSet> Mt32Synth::ScanRomSets()
{
    QVector<RomSet> out;

    const QString dir = InstallDir();
    QDir d(dir);
    if (!d.exists())
        return out;

    // Identify every candidate file once, by content. mt32emu hashes the file
    // and hands back a pointer into its own static table, so the same ROM
    // always yields the same pointer and machines can be matched by identity
    // below rather than by name.
    struct Identified {
        QString path;
        const MT32Emu::ROMInfo* info = nullptr;
    };
    QVector<Identified> found;

    const QFileInfoList files = d.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& fi : files) {
        if (fi.size() == 0 || fi.size() > kMaxRomBytes)
            continue;

        MT32Emu::FileStream fs;
        if (!fs.open(fi.absoluteFilePath().toLocal8Bit().constData()))
            continue;

        if (const MT32Emu::ROMInfo* info = MT32Emu::ROMInfo::getROMInfo(&fs)) {
            // Split dumps need merging with their other half before they are
            // usable; that is not supported here, and quietly treating one half
            // as a whole ROM would fail later with nothing to go on.
            if (info->pairType == MT32Emu::ROMInfo::Full)
                found.append({ fi.absoluteFilePath(), info });
            else
                qDebug() << "[Mt32Synth] half ROM, needs merging - ignored:" << fi.fileName();
        }
        fs.close();
    }

    if (found.isEmpty())
        return out;

    MT32Emu::Bit32u machineCount = 0;
    const MT32Emu::MachineConfiguration* const* machines =
        MT32Emu::MachineConfiguration::getAllMachineConfigurations(&machineCount);

    for (MT32Emu::Bit32u m = 0; m < machineCount; ++m) {
        const MT32Emu::MachineConfiguration* mc = machines[m];
        if (!mc) continue;

        MT32Emu::Bit32u infoCount = 0;
        const MT32Emu::ROMInfo* const* compatible = mc->getCompatibleROMInfos(&infoCount);
        if (!compatible) continue;

        QString control, pcm;
        for (const Identified& f : found) {
            bool isCompatible = false;
            for (MT32Emu::Bit32u i = 0; i < infoCount; ++i) {
                if (compatible[i] == f.info) { isCompatible = true; break; }
            }
            if (!isCompatible)
                continue;

            if (f.info->type == MT32Emu::ROMInfo::Control && control.isEmpty())
                control = f.path;
            else if (f.info->type == MT32Emu::ROMInfo::PCM && pcm.isEmpty())
                pcm = f.path;
        }

        if (!control.isEmpty() && !pcm.isEmpty()) {
            const QString id = QString::fromLatin1(mc->getMachineID());
            out.append({ id, PrettyMachineName(id), control, pcm });
        }
    }

    return out;
}

QString Mt32Synth::UnavailableReason()
{
    EnsureInstallDir();

    if (!ScanRomSets().isEmpty())
        return QString();

    return LSTR(
        u8"MT-32 롬을 찾지 못했습니다.\n\n"
        u8"MT32ROMs 폴더에 컨트롤 롬과 PCM 롬을 한 쌍 넣어주세요.\n"
        u8"(예: MT32_CONTROL.ROM + MT32_PCM.ROM)\n\n"
        u8"에뮬레이터는 함께 배포되지만 롬은 롤랜드의 저작물이라\n"
        u8"직접 구하셔야 합니다. 자세한 내용은 그 폴더의 설명서를 보세요.",
        u8"No MT-32 ROMs were found.\n\n"
        u8"Put a matching control ROM and PCM ROM in the MT32ROMs folder\n"
        u8"(for example MT32_CONTROL.ROM and MT32_PCM.ROM).\n\n"
        u8"The emulator ships with jmp, but the ROMs are Roland's copyrighted\n"
        u8"material and have to be obtained separately. See the note in that\n"
        u8"folder.");
}

Mt32Synth::Mt32Synth() = default;

Mt32Synth::~Mt32Synth()
{
    Close();
}

void Mt32Synth::Destroy(Loaded& L)
{
    delete L.pConv;
    L.pConv = nullptr;

    if (L.pSynth) {
        L.pSynth->close();
        delete L.pSynth;
        L.pSynth = nullptr;
    }

    // The images hold the File objects, so free the image first and the file
    // after - the other order reads freed memory.
    if (L.pControl) { MT32Emu::ROMImage::freeROMImage(L.pControl); L.pControl = nullptr; }
    if (L.pPCM)     { MT32Emu::ROMImage::freeROMImage(L.pPCM);     L.pPCM     = nullptr; }
    delete L.pControlFile; L.pControlFile = nullptr;
    delete L.pPCMFile;     L.pPCMFile     = nullptr;
}

bool Mt32Synth::Open(const QString& romSetId)
{
    m_sError.clear();

    const QVector<RomSet> sets = ScanRomSets();
    if (sets.isEmpty()) {
        m_sError = UnavailableReason();
        return false;
    }

    // Named set if it is still there, otherwise the first - which is the oldest
    // machine present, since munt lists them in that order.
    RomSet chosen = sets.first();
    if (!romSetId.isEmpty()) {
        for (const RomSet& s : sets) {
            if (s.id == romSetId) { chosen = s; break; }
        }
    }

    // Everything is built before the swap so the audio thread is never held up
    // by a megabyte of ROM being read off disk.
    Loaded next;
    next.pControlFile = new MT32Emu::FileStream();
    next.pPCMFile     = new MT32Emu::FileStream();

    auto fail = [&](const QString& why) {
        m_sError = why;
        Destroy(next);
        return false;
    };

    if (!next.pControlFile->open(chosen.controlPath.toLocal8Bit().constData()))
        return fail(LSTR(u8"컨트롤 롬을 열지 못했습니다: ", u8"Could not open the control ROM: ") + chosen.controlPath);
    if (!next.pPCMFile->open(chosen.pcmPath.toLocal8Bit().constData()))
        return fail(LSTR(u8"PCM 롬을 열지 못했습니다: ", u8"Could not open the PCM ROM: ") + chosen.pcmPath);

    next.pControl = MT32Emu::ROMImage::makeROMImage(next.pControlFile);
    next.pPCM     = MT32Emu::ROMImage::makeROMImage(next.pPCMFile);
    if (!next.pControl || !next.pPCM || !next.pControl->getROMInfo() || !next.pPCM->getROMInfo())
        return fail(LSTR(u8"롬 파일을 인식하지 못했습니다.", u8"The ROM files were not recognised."));

    next.pSynth = new MT32Emu::Synth();
    if (!next.pSynth->open(*next.pControl, *next.pPCM))
        return fail(LSTR(u8"MT-32 엔진을 초기화하지 못했습니다.", u8"The MT-32 engine failed to start."));

    // GOOD is munt's own default and is inaudibly close to BEST while costing a
    // fraction of it - this runs inside the audio callback.
    next.pConv = new MT32Emu::SampleRateConverter(
        *next.pSynth, kOutputSampleRate, MT32Emu::SamplerateConversionQuality_GOOD);

    Loaded previous;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        previous = m_L;
        m_L      = next;
        m_bOpen.store(true, std::memory_order_release);
    }
    Destroy(previous);

    m_sRomId    = chosen.id;
    m_sRomLabel = chosen.label;
    qDebug() << "[Mt32Synth] opened" << chosen.label << "control:" << chosen.controlPath;
    return true;
}

void Mt32Synth::Close()
{
    Loaded previous;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_bOpen.store(false, std::memory_order_release);
        previous = m_L;
        m_L      = Loaded{};
    }
    Destroy(previous);

    m_sRomId.clear();
    m_sRomLabel.clear();
}

void Mt32Synth::SendShort(unsigned char status, unsigned char data1, unsigned char data2)
{
    if (!m_bOpen.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_L.pSynth) return;

    const MT32Emu::Bit32u msg =
        MT32Emu::Bit32u(status) | (MT32Emu::Bit32u(data1) << 8) | (MT32Emu::Bit32u(data2) << 16);
    m_L.pSynth->playMsg(msg);
}

void Mt32Synth::SendSysEx(const std::vector<unsigned char>& framed)
{
    if (!m_bOpen.load(std::memory_order_acquire) || framed.empty())
        return;

    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_L.pSynth) return;

    m_L.pSynth->playSysex(framed.data(), MT32Emu::Bit32u(framed.size()));
}

void Mt32Synth::AllSoundOff()
{
    if (!m_bOpen.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_L.pSynth) return;

    // CC 0x7B on all channels. The MT-32 has eight parts plus rhythm, but
    // sending sixteen costs nothing and covers whatever the part layout is.
    for (unsigned char ch = 0; ch < 16; ++ch) {
        const MT32Emu::Bit32u msg =
            MT32Emu::Bit32u(0xB0 | ch) | (MT32Emu::Bit32u(0x7B) << 8);
        m_L.pSynth->playMsg(msg);
    }
}

void Mt32Synth::SetMasterVolume(int volume0to127)
{
    if (!m_bOpen.load(std::memory_order_acquire))
        return;

    // The MT-32's master volume is 0-100 in its System Area at 0x10 0x00 0x16.
    const int v = qBound(0, (qBound(0, volume0to127, 127) * 100) / 127, 100);

    std::vector<unsigned char> sysex = {
        0xF0, 0x41, 0x10, 0x16, 0x12,     // Roland, dev 17, MT-32, DT1
        0x10, 0x00, 0x16,                 // address: System Area / MASTER VOL
        (unsigned char) v
    };
    // Roland checksum: the address and data bytes sum to a multiple of 128.
    int sum = 0x10 + 0x00 + 0x16 + v;
    sysex.push_back((unsigned char) ((128 - (sum & 0x7F)) & 0x7F));
    sysex.push_back(0xF7);

    SendSysEx(sysex);
}

void Mt32Synth::Render(float* pOutput, unsigned int nFrames)
{
    std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);

    // A ROM change is in progress. One buffer of silence beats blocking the
    // audio callback behind a file read.
    if (!lock.owns_lock() || !m_L.pConv) {
        memset(pOutput, 0, size_t(nFrames) * 2 * sizeof(float));
        return;
    }

    m_L.pConv->getOutputSamples(pOutput, nFrames);
}

QString Mt32Synth::DisplayText() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_L.pSynth)
        return QString();

    char buf[21] = {};
    m_L.pSynth->getDisplayState(buf, false);
    return QString::fromLatin1(buf);
}

bool Mt32Synth::MidiLed() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_L.pSynth)
        return false;

    char buf[21] = {};
    return m_L.pSynth->getDisplayState(buf, false);
}
