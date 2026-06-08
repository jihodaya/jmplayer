#include "okabackend.h"
#include "okafilehandler.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <algorithm>
#include <sstream>

#ifndef _byteswap_ulong
#define _byteswap_ulong(x) __builtin_bswap32(x)
#endif
#ifndef _byteswap_ushort
#define _byteswap_ushort(x) __builtin_bswap16(x)
#endif

// Helper to read variable length values from stream (identical to MidiPlayer)
static unsigned long readVarLen(std::istream &file)
{
    unsigned long value = 0;
    unsigned char byte;
    for (int i = 0; i < 4; i++) {
        file.read(reinterpret_cast<char*>(&byte), 1);
        value = (value << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) break;
    }
    return value;
}

CPlayer* OkaBackend::factory(Copl* opl)
{
    return new OkaBackend(opl);
}

OkaBackend::OkaBackend(Copl* opl)
    : CcomposerBackend(opl)
    , m_eventIdx(0)
    , m_tickHz(120.0f)
    , m_ticksPerQuarter(120)
    , m_currentTempo(500000) // 120 BPM
    , m_userTempoScale(100)
    , m_userKeyTranspose(0)
    , m_totalSongTicks(0)
    , m_currentTick(0)
{
    for (int i = 0; i < 18; ++i) {
        m_voiceSlot[i] = -1;
        m_initialProgram[i] = -1;
        m_voiceNote[i] = -1;
    }
}

int OkaBackend::getCurrentBpm() const
{
    unsigned long tempo = tempoAtTick(m_currentTick);
    double bpm = 60000000.0 / (double)tempo;
    double scaledBpm = bpm * ((double)m_userTempoScale / 100.0);
    return (int)qRound(scaledBpm);
}

float OkaBackend::calculateTickHz(uint64_t tick) const
{
    unsigned long tempo = tempoAtTick(tick);
    double scale = (double)m_userTempoScale / 100.0;
    // Hz = (1,000,000 * ticksPerQuarter / tempo) * scale
    float rate = (float)(1000000.0 * (double)m_ticksPerQuarter / (double)tempo) * scale;
    if (rate < 1.0f) rate = 120.0f;
    return rate;
}

unsigned long OkaBackend::tempoAtTick(uint64_t tick) const
{
    unsigned long tempo = 500000;
    for (const auto& tc : m_tempoMap) {
        if (tc.tick > tick) break;
        tempo = tc.tempo;
    }
    return tempo;
}

bool OkaBackend::resolveBnkPatches(const std::string& songFilename)
{
    std::cout << "      [resolveBnkPatches] starting..." << std::endl << std::flush;
    CProvider_Filesystem fp;
    std::cout << "      [resolveBnkPatches] CProvider_Filesystem fp created." << std::endl << std::flush;
    // Search STAND.BNK under standard locations (identical to GybBackend)
    QString songPath = QString::fromStdString(songFilename);
    QFileInfo songInfo(songPath);
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    // 1. User-selected external bank (BNK button) takes top priority.
    if (!m_externalBankPath.isEmpty())
        candidates << m_externalBankPath;
    // 2. Standard Oksori bank locations (default fallback — unchanged behaviour).
    candidates << songInfo.absolutePath() + "/STANDARD.BNK"
               << songInfo.absolutePath() + "/standard.bnk"
               << appDir + "/STANDARD.BNK"
               << appDir + "/dos/STANDARD.BNK"
               << "D:/py/midi-k-c260415/STANDARD.BNK"
               << "D:/py/midi-k-c260415/dos/STANDARD.BNK";

    SBnkHeader header;
    binistream* bnkFile = nullptr;
    QString chosen;
    for (const QString& c : candidates) {
        if (!QFile::exists(c)) continue;
        QByteArray cBytes = QFile::encodeName(c);
        binistream* f = fp.open(cBytes.constData());
        if (!f) continue;
        // Accept only a candidate that parses as a Visual Composer BNK. If the
        // user-selected external bank is invalid, fall through to STANDARD.BNK.
        if (load_bnk_info(f, header)) {
            bnkFile = f; chosen = c; break;
        }
        qWarning() << "[OkaBackend] load_bnk_info failed, skipping:" << c;
        fp.close(f);
    }
    if (!bnkFile) {
        qWarning() << "[OkaBackend] No usable BNK found for" << songPath;
        return false;
    }

    m_bankName = QFileInfo(chosen).fileName();
    m_slotToInstIndex.clear();
    int hit = 0;
    for (const QString& slot : m_slotNames) {
        if (slot.isEmpty()) {
            m_slotToInstIndex.append(-1);
            continue;
        }
        std::string s = slot.toStdString();
        int idx = load_bnk_instrument(bnkFile, header, s);
        m_slotToInstIndex.append(idx);
        if (idx >= 0) ++hit;
    }
    fp.close(bnkFile);
    qDebug() << "[OkaBackend] STAND.BNK loaded:" << chosen
             << "slots=" << m_slotNames.size() << "hit=" << hit;
    return true;
}

bool OkaBackend::parseMidiData(const QByteArray& midiData)
{
    m_events.clear();
    m_tempoMap.clear();

    std::istringstream fileStream(std::string(midiData.constData(), midiData.size()));
    std::istream &file = fileStream;

    char mthd[4];
    file.read(mthd, 4);
    if (strncmp(mthd, "MThd", 4) != 0) return false;

    unsigned long headerLength = 0;
    file.read(reinterpret_cast<char*>(&headerLength), 4);
    headerLength = _byteswap_ulong(headerLength);

    unsigned short format = 0;
    file.read(reinterpret_cast<char*>(&format), 2);
    format = _byteswap_ushort(format);

    unsigned short numTracks = 0;
    file.read(reinterpret_cast<char*>(&numTracks), 2);
    numTracks = _byteswap_ushort(numTracks);

    unsigned short timeDivision = 0;
    file.read(reinterpret_cast<char*>(&timeDivision), 2);
    timeDivision = _byteswap_ushort(timeDivision);

    if (timeDivision & 0x8000) return false; // SMTP not supported
    m_ticksPerQuarter = timeDivision;

    // Tempo map init
    TempoChange initialTempo;
    initialTempo.tick = 0;
    initialTempo.tempo = 500000;
    m_tempoMap.push_back(initialTempo);

    unsigned long maxTick = 0;

    for (int trackNum = 0; trackNum < numTracks; ++trackNum) {
        char mtrk[4];
        file.read(mtrk, 4);
        if (strncmp(mtrk, "MTrk", 4) != 0) continue;

        unsigned long trackLength = 0;
        file.read(reinterpret_cast<char*>(&trackLength), 4);
        trackLength = _byteswap_ulong(trackLength);

        std::streampos trackEnd = file.tellg() + std::streampos(trackLength);
        unsigned char runningStatus = 0;
        unsigned long absoluteTick = 0;

        while (file.tellg() < trackEnd && file.good()) {
            unsigned long deltaTime = readVarLen(file);
            absoluteTick += deltaTime;

            OkaMidiEvent event;
            event.absoluteTick = absoluteTick;
            event.isMeta = false;

            unsigned char status;
            file.read(reinterpret_cast<char*>(&status), 1);
            if (status < 0x80) {
                status = runningStatus;
                file.seekg(-1, std::ios::cur);
            } else {
                runningStatus = status;
            }
            event.status = status;

            if (status == 0xFF) {
                event.isMeta = true;
                file.read(reinterpret_cast<char*>(&event.data1), 1); // meta type
                unsigned long length = readVarLen(file);
                event.metaData.resize(length);
                if (length > 0) {
                    file.read(reinterpret_cast<char*>(event.metaData.data()), length);
                }

                // Tempo change track
                if (event.data1 == 0x51 && length >= 3) {
                    unsigned char m0 = (unsigned char)event.metaData[0];
                    unsigned char m1 = (unsigned char)event.metaData[1];
                    unsigned char m2 = (unsigned char)event.metaData[2];
                    unsigned long newTempo = (m0 << 16) | (m1 << 8) | m2;
                    TempoChange tc;
                    tc.tick = absoluteTick;
                    tc.tempo = newTempo;
                    m_tempoMap.push_back(tc);
                }

                if (event.data1 == 0x2F) {
                    // End of track
                    break;
                }
            } else if (status == 0xF0 || status == 0xF7) {
                // SysEx
                unsigned long length = readVarLen(file);
                file.seekg(length, std::ios::cur);
            } else {
                file.read(reinterpret_cast<char*>(&event.data1), 1);
                if ((status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0) {
                    file.read(reinterpret_cast<char*>(&event.data2), 1);
                } else {
                    event.data2 = 0;
                }
                m_events.push_back(event);
            }
        }
        maxTick = std::max(maxTick, absoluteTick);
    }

    m_totalSongTicks = maxTick;

    // Sort tempo map
    std::sort(m_tempoMap.begin(), m_tempoMap.end(), [](const TempoChange& a, const TempoChange& b) {
        return a.tick < b.tick;
    });

    // Sort midi events
    std::sort(m_events.begin(), m_events.end(), [](const OkaMidiEvent& a, const OkaMidiEvent& b) {
        return a.absoluteTick < b.absoluteTick;
    });

    return true;
}

bool OkaBackend::load(const std::string& filename, const CFileProvider& fp)
{
    Q_UNUSED(fp);
    return load(filename);
}

bool OkaBackend::load(const std::string& filename)
{
    std::cout << "   [OkaBackend] load starting..." << std::endl << std::flush;
    QString qf = QString::fromStdString(filename);
    QByteArray midiData = OkaFileHandler::extractMidiData(qf);
    if (midiData.isEmpty()) {
        std::cout << "   [OkaBackend] load failed: midiData is empty" << std::endl << std::flush;
        return false;
    }
    std::cout << "   [OkaBackend] midiData extracted." << std::endl << std::flush;
    
    if (!parseMidiData(midiData)) {
        std::cout << "   [OkaBackend] load failed: parseMidiData failed" << std::endl << std::flush;
        return false;
    }
    std::cout << "   [OkaBackend] parseMidiData completed." << std::endl << std::flush;

    // Scan for initial program change events for each channel
    for (int i = 0; i < 18; ++i) m_initialProgram[i] = -1;
    for (const auto& ev : m_events) {
        if (!ev.isMeta) {
            unsigned char msgType = ev.status & 0xF0;
            int channel = ev.status & 0x0F;
            if (channel < 18 && msgType == 0xC0) {
                if (m_initialProgram[channel] == -1) {
                    m_initialProgram[channel] = ev.data1;
                }
            }
        }
    }
    
    m_title = OkaFileHandler::extractTitle(qf);
    if (m_title.isEmpty()) m_title = QFileInfo(qf).fileName();
    
    m_slotNames = OkaFileHandler::extractInstrumentNames(qf);
    std::cout << "   [OkaBackend] slotNames extracted, count = " << m_slotNames.size() << std::endl << std::flush;
    
    std::cout << "   [OkaBackend] calling resolveBnkPatches..." << std::endl << std::flush;
    if (!resolveBnkPatches(filename)) {
        std::cout << "   [OkaBackend] resolveBnkPatches returned false." << std::endl << std::flush;
        m_slotToInstIndex.clear();
        for (int i = 0; i < m_slotNames.size(); ++i) m_slotToInstIndex.append(-1);
    }
    std::cout << "   [OkaBackend] resolveBnkPatches completed." << std::endl << std::flush;
    
    rewind(0);
    std::cout << "   [OkaBackend] rewind completed. returning true." << std::endl << std::flush;
    return true;
}

void OkaBackend::rewind(int subsong)
{
    CcomposerBackend::rewind(subsong);

    // Enable rhythm mode to match percussive voice numbering
    // and correct drum timbre for MIDI channels 10 mapping.
    SetRhythmMode(1);

    m_currentTick = 0;
    m_eventIdx = 0;
    m_currentTempo = 500000;
    m_tickHz = calculateTickHz(0);

    for (int i = 0; i < 18; ++i) {
        m_voiceSlot[i] = m_initialProgram[i];
        m_voiceNote[i] = -1;
    }
    for (int i = 0; i < 16; ++i) {
        m_channelVolume[i] = 100; // MIDI default CC7
        m_lastVel[i]       = 100;
    }
    for (int i = 0; i < 18; ++i) m_voiceAttack[i] = 0;

    // Initialize voices volume and pitch bend
    for (int i = 0; i < 11; ++i) {
        SetVolume(i, 100);
        ChangePitch(i, 0x2000); // 8192 centered
    }
}

bool OkaBackend::update()
{
    bool alive = advanceOneTick();
    m_tickHz = calculateTickHz(m_currentTick);
    return alive;
}

bool OkaBackend::advanceOneTick()
{
    if (m_eventIdx >= m_events.size() && m_currentTick >= (uint64_t)m_totalSongTicks) {
        return false;
    }

    while (m_eventIdx < m_events.size()) {
        const auto& ev = m_events[m_eventIdx];
        if (ev.absoluteTick > m_currentTick) break;

        unsigned char status = ev.status;
        unsigned char msgType = status & 0xF0;
        int channel = status & 0x0F;

        // Skip channel 11 (index 10) which is Johab lyrics note-on marker
        // Also limit channel to < 11 (MAX_VOICES) to prevent CcomposerBackend heap corruption
        if (channel < 11 && channel != 10) {
            switch (msgType) {
                case 0x90: { // Note On
                    int note = ev.data1 + m_userKeyTranspose;
                    if (note < 0) note = 0;
                    if (note > 127) note = 127;
                    int vel = ev.data2;

                    // MIDI NoteOn with velocity 0 is treated as NoteOff
                    if (vel > 0) {
                        if (m_voiceNote[channel] >= 0) {
                            NoteOff(channel);
                        }
                        // OKA per-note volume = note velocity, applied directly.
                        // In the Oksori format the note velocity carries the
                        // per-channel mix level (CC7 == velocity), so we must NOT
                        // multiply the two (that would attenuate quadratically and
                        // flatten the mix). Writing SetVolume every note also makes
                        // the channel-monitor levels track real loudness, matching
                        // GYB which writes its volume stream per note.
                        int eff = vel;
                        if (eff > 127) eff = 127;
                        SetVolume(channel, (uint8_t)eff);
                        m_lastVel[channel] = vel;
                        NoteOn(channel, note);
                        m_voiceNote[channel] = note;
                        // Record the onset for the level meter (peak = velocity).
                        if (vel > m_voiceAttack[channel]) m_voiceAttack[channel] = vel;
                    } else {
                        if (m_voiceNote[channel] >= 0) {
                            NoteOff(channel);
                            m_voiceNote[channel] = -1;
                        }
                    }
                    break;
                }
                case 0x80: { // Note Off
                    if (m_voiceNote[channel] >= 0) {
                        NoteOff(channel);
                        m_voiceNote[channel] = -1;
                    }
                    break;
                }
                case 0xC0: { // Program Change
                    int prog = ev.data1;
                    if (prog >= 0 && prog < m_slotToInstIndex.size()) {
                        int instIdx = m_slotToInstIndex[prog];
                        if (instIdx >= 0) {
                            int savedNote = m_voiceNote[channel];
                            if (savedNote >= 0) {
                                NoteOff(channel);
                            }
                            SetInstrument(channel, instIdx);
                            if (savedNote >= 0) {
                                // Re-apply the held note's volume, since
                                // SetInstrument resets the carrier TL.
                                int eff = m_lastVel[channel];
                                if (eff > 127) eff = 127;
                                SetVolume(channel, (uint8_t)eff);
                                NoteOn(channel, savedNote);
                            }
                        }
                    }
                    m_voiceSlot[channel] = prog;
                    break;
                }
                case 0xB0: { // Control Change
                    if (ev.data1 == 7) { // Channel Volume
                        m_channelVolume[channel] = ev.data2;
                        // CC7 carries the same per-channel level as note velocity in
                        // the Oksori format, so apply it directly (no multiply) to the
                        // currently sounding note for mid-note expression (e.g. ch7).
                        m_lastVel[channel] = ev.data2;
                        if (m_voiceNote[channel] >= 0) {
                            int eff = ev.data2;
                            if (eff > 127) eff = 127;
                            SetVolume(channel, (uint8_t)eff);
                        }
                    }
                    break;
                }
                case 0xE0: { // Pitch Bend
                    // MIDI pitch bend is 14-bit: data1 | (data2 << 7).
                    // AdPlug ChangePitch expects 0..0x3FFF (center 0x2000).
                    int bend = ev.data1 | (ev.data2 << 7);
                    ChangePitch(channel, (uint16_t)bend);
                    break;
                }
            }
        }
        m_eventIdx++;
    }

    m_currentTick++;
    return (m_eventIdx < m_events.size() || m_currentTick < (uint64_t)m_totalSongTicks);
}

std::string OkaBackend::getvoiceinstrument(unsigned int voice)
{
    if (voice >= 18) return std::string();
    int slot = m_voiceSlot[voice];
    if (slot >= 0 && slot < m_slotNames.size()) {
        return m_slotNames[slot].toStdString();
    }
    return std::string();
}
