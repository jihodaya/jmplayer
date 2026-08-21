#include "okabackend.h"
#include "uistrings.h"
#include "okafilehandler.h"
#include "bnkfill.h"
#include <QFile>
#include <QHash>
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
    CProvider_Filesystem fp;
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

// See GybBackend::loadEmbeddedPatches - identical idea, identical record
// layout. An .OKA keeps the table after its MIDI instead of after the channel
// data, but the 38-byte records are the same thing.
void OkaBackend::FixRhythmFrequency(int voice)
{
    // Same octave correction as GybBackend - NORE45 shares GAYOBANG's OPL
    // driver. FUN_19aa_1162 is FUN_255a_1222 function for function: it writes
    // 0xBD with the rhythm bit, sets register 8 to 0x40, then stores note 24 on
    // channel 8 and note 31 on channel 7 and takes the channel count to 11.
    // Those notes are 65.4 Hz and 98.0 Hz through the driver's note table;
    // AdPlug's kTomTomNote / kSnareNote land an octave up.
    if (voice < 7 || voice > 10) return;
    opl->write(0xA7, 0x05); opl->write(0xB7, 0x0A);   // fnum 517, block 2
    opl->write(0xA8, 0xB2); opl->write(0xB8, 0x06);   // fnum 690, block 1
}

void OkaBackend::SetChannelVolume(int voice, uint8_t volume)
{
    // Channel volume including the modulator of an additive patch. NORE45's TL
    // writer carries GAYOBANG's condition unchanged - scale when the operator is
    // the carrier, or its own fm_type is 0, or it is a rhythm drum operator -
    // while AdPlug's SetVolume only ever touches the carrier.
    SetVolume(voice, volume);
    if (voice < 0 || voice >= 9 || voice >= 7) return;   // 7..10 are one operator each
    if (!m_voiceAdditive[voice]) return;

    static const int kOpTable[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };
    const uint8_t ksltl = m_voiceModKsltl[voice];
    uint16_t level = 63 - (ksltl & 0x3F);
    level = (uint16_t)(volume * level);
    level += level + 127;
    level = 63 - (level / 254);
    opl->write(0x40 + kOpTable[voice], (int)((ksltl & 0xC0) | level));
}

void OkaBackend::NoteInstrument(int voice, int slot, int instIdx)
{
    SetInstrument(voice, instIdx);
    if (voice < 0 || voice >= 18) return;
    m_voiceAdditive[voice] = false;
    m_voiceModKsltl[voice] = 0;
    if (slot >= 0 && slot < m_instParams.size()) {
        const QByteArray& q = m_instParams[slot];
        if (q.size() >= kEmbeddedParamLen) {
            m_voiceModKsltl[voice] = (uint8_t)(((uint8_t)q[0] << 6) | ((uint8_t)q[8] & 0x3F));
            m_voiceAdditive[voice] = ((uint8_t)q[12] == 0);
        }
    }
}


// The patch a channel starts on, the way FUN_255a_101b sets every channel up
// before a song plays: the melodic default for 0..5 and the rhythm kit for
// 6..10. Returns an instrument index usable with SetInstrument(), or -1.
int OkaBackend::builtinInstIndex(int voice)
{
    const int which = (voice >= 6 && voice <= 10) ? voice - 5 : 0;
    if (m_builtinInst[which] < 0) {
        QByteArray p = builtinDefaultInstrument(which);
        m_builtinInst[which] = load_instrument_data(
            reinterpret_cast<uint8_t*>(p.data()), kEmbeddedParamLen);
    }
    return m_builtinInst[which];
}

bool OkaBackend::loadEmbeddedPatches()
{
    if (m_slotNames.isEmpty() || m_instParams.isEmpty())
        return false;

    m_slotToInstIndex.clear();
    int loaded = 0;
    for (const QByteArray& params : m_instParams) {
        if (params.size() < kEmbeddedParamLen) {
            m_slotToInstIndex.append(-1);
            continue;
        }
        bool allZero = true;
        for (int k = 0; k < params.size(); ++k) if (params[k]) { allZero = false; break; }
        // Empty record: the voice keeps its previous patch. NORE45 shares
        // GAYOBANG's driver and its FUN_1bd4_0142 loads a built-in instead;
        // see the long note in GybBackend::loadEmbeddedPatches() for why the
        // measurement went the other way and what would settle it.
        if (allZero) { m_slotToInstIndex.append(-1); continue; }
        // Waveform is two bits in the original loader; see GybBackend.
        QByteArray p2 = params;
        p2[26] = (char)(p2[26] & 3);
        p2[27] = (char)(p2[27] & 3);
        const int idx = load_instrument_data(
            reinterpret_cast<uint8_t*>(const_cast<char*>(p2.constData())),
            kEmbeddedParamLen);
        m_slotToInstIndex.append(idx);
        if (idx >= 0) ++loaded;
    }

    // Shown in the channel monitor so it is obvious the song is not
    // leaning on any external bank.
    m_bankName = "embedded bank";
    qDebug() << "[OkaBackend] embedded instruments:" << loaded << "/"
             << m_slotNames.size();
    return loaded > 0;
}

bool OkaBackend::load(const std::string& filename, const CFileProvider& fp)
{
    Q_UNUSED(fp);
    return load(filename);
}

bool OkaBackend::load(const std::string& filename)
{
    QString qf = QString::fromStdString(filename);
    QByteArray midiData = OkaFileHandler::extractMidiData(qf);
    if (midiData.isEmpty()) {
        return false;
    }
    
    if (!parseMidiData(midiData)) {
        return false;
    }

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
    m_instParams = OkaFileHandler::extractInstrumentParams(qf);

    // .OKA plays its own embedded instruments and nothing else - no external
    // bank, ever, whatever is registered elsewhere in the program (user
    // decision, 2026-08-11). NORE45 does resolve names from STANDARD.BNK the
    // way GAYOBANG does, but .OKA sounds right on the embedded table as it is,
    // so we deliberately do not follow it here. A file with no usable table
    // falls through to AdPlug's default instrument rather than to a bank.
    // NORE45's own bank first, GAYOBANG's second. Switching .OKA to GAYO-first
    // to match .GYB was tried on 2026-08-21 and reverted: measured over the
    // whole library it changes 20 .OKA files - none of which has a .GYB twin -
    // and leaves all 28 paired songs bit-identical, because those .OKA files
    // carry their instruments embedded and read no bank at all. It cannot be
    // what makes a .GYB and its .OKA sound different.
    fillEmptyInstrumentSlots(m_slotNames, m_instParams, qf,
                             kEmbeddedParamLen, "[OkaBackend]", BankOrder::Nore45,
                             m_externalBankPath);
    if (loadEmbeddedPatches()) {
    } else {
        m_slotToInstIndex.clear();
        for (int i = 0; i < m_slotNames.size(); ++i) m_slotToInstIndex.append(-1);
    }
    
    rewind(0);
    return true;
}

void OkaBackend::rewind(int subsong)
{
    CcomposerBackend::rewind(subsong);

    // Enable rhythm mode to match percussive voice numbering
    // and correct drum timbre for MIDI channels 10 mapping.
    SetRhythmMode(1);

    // NOTE-SEL, as FUN_19aa_1162 does; AdPlug's init leaves it clear.
    opl->write(0x08, 0x40);

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
        SetChannelVolume(i, 100);
        ChangePitch(i, 0x2000); // 8192 centered
    }

    // Every channel starts on the player's own built-in patch, the way
    // FUN_19aa's reset does, so a song whose first program change points at an
    // empty record still has something to sound with. From then on such a
    // change loads nothing and the channel keeps what it has - see bnkfill.cpp.
    for (int i = 0; i < 11; ++i) {
        const int prog = m_initialProgram[i];
        int instIdx = (prog >= 0 && prog < m_slotToInstIndex.size())
                    ? m_slotToInstIndex[prog] : -1;
        if (instIdx < 0) instIdx = builtinInstIndex(i);
        if (instIdx >= 0) NoteInstrument(i, prog, instIdx);
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

        // Channels 0-10 map 1:1 onto AdPlug's 11 voices; anything above would
        // index past MAX_VOICES and corrupt CcomposerBackend's caches.
        //
        // Channel 10 used to be dropped here as a "Johab lyrics marker". It is
        // not - it is the hi-hat. In rhythm mode AdPlug's voices 6-10 are bass
        // drum, snare, tom, cymbal and hi-hat, and every .OKA follows exactly
        // that layout: SOVIRGIN.OKA's channel 10 carries 1,488 note-ons on GM
        // notes 42/46 (closed/open hi-hat) and 890 program changes alternating
        // between its CLHIHAT0 and OPHIHAT slots. Its GYB twin plays the same
        // channel (GybBackend loops ch 0..10), which is why only .OKA came out
        // sounding wrong. Lyric timing comes from the trailing sync block, not
        // from this channel - see MainWindow's OKA lyric path.
        if (channel < 11) {
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
                        SetChannelVolume(channel, (uint8_t)eff);
                        m_lastVel[channel] = vel;
                        NoteOn(channel, note);
                        FixRhythmFrequency(channel);
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
                            // Load the instrument; do NOT re-strike a sounding
                            // note. Keying the held note off and on again turns
                            // every mid-note instrument change into an extra
                            // hit - see the same change in GybBackend, where it
                            // was the second sound behind SOVIRGIN's closing
                            // bell. NORE45 settles it: its 0xC0 handler
                            // (FUN_1bd4_0142) calls the instrument loader and
                            // nothing else.
                            //
                            // It matters more here than in GYB. The Oksori
                            // format barely uses note-offs - 901 against
                            // 119,984 note-ons across the library, 0.75% - so a
                            // channel's note stays held until the next one, and
                            // 3,282 of 4,018 program changes (82%) landed on a
                            // sounding voice. That many stray hits: 60 of 63
                            // songs change, BOOCKBOY loses a peak of 8,889 at
                            // its ending and TJA one of 16,063 mid-song.
                            //
                            // The volume re-apply that used to follow is not
                            // needed either: SetInstrument writes the carrier
                            // TL through GetKSLTL(), which already folds in the
                            // volume that note-on set from the velocity.
                            NoteInstrument(channel, prog, instIdx);
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
                            SetChannelVolume(channel, (uint8_t)eff);
                        }
                    }
                    break;
                }
                case 0xE0: { // Pitch Bend
                    // NORE45 does not hand the raw 14-bit bend to the AdLib
                    // library - it widens it first. Its handler is
                    //
                    //   ChangePitch(voice, ((bend - 0x2000) * 5 + 0x3ffc) / 2)
                    //
                    // (NORE45.EXE.c FUN_1bd4_0105, written there as an OR with
                    // 0xe000 because the deviation is formed in 16-bit
                    // arithmetic). So a deviation is scaled by 2.5 and
                    // re-centred on 8190.
                    //
                    // Passing the raw value through, as this did, made every
                    // .OKA bend 2.5x too shallow. The song data is not the
                    // problem: a .GYB and its .OKA twin hold the same music,
                    // and their bend streams are the same curve at different
                    // scales - GYB stores a 0..20 byte that GAYOBANG multiplies
                    // by 0x333 (819/step, GAYOBANG.c:30115), the .OKA converter
                    // wrote 0x2000 + (raw-10)*300 (300/step). Only after this
                    // x2.5 does the .OKA reach 750/step and land next to the
                    // .GYB, which is what the two originals actually sound
                    // like. SOVIRGIN's channels 0 and 1 are a detuned unison,
                    // so the error was plainly audible as mistuning.
                    const int bend = ev.data1 | (ev.data2 << 7);
                    int scaled = ((bend - 0x2000) * 5 + 0x3ffc) / 2;
                    if (scaled < 0)      scaled = 0;      // real files stay
                    if (scaled > 0x3FFF) scaled = 0x3FFF; // well inside range
                    ChangePitch(channel, (uint16_t)scaled);
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
