#include "gybbackend.h"
#include "uistrings.h"
#include "gybfilehandler.h"
#include "bnkfill.h"
#include "settingsmanager.h"
#include <QFile>
#include <QHash>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <algorithm>
#include <cstring>

CPlayer* GybBackend::factory(Copl* opl)
{
    return new GybBackend(opl);
}

GybBackend::GybBackend(Copl* opl)
    : CcomposerBackend(opl)
    , m_tickHz(60.0f)
    , m_basicTempo(100)
    , m_tbDiv(4)
    , m_effectiveBpmX100(10000)
    , m_keyTranspose(0)
    , m_userTempoScale(100)
    , m_userKeyTranspose(0)
    , m_totalSongTicks(0)
    , m_currentTick(0)
    , m_rhythmMode(true)
{
    for (int i = 0; i < 18; ++i) m_voiceSlot[i] = -1;
}

int GybBackend::globalTempoScale(uint64_t tick) const
{
    // Global events are tempo-scale percentages applied to header[0x34]:
    // value 100 = normal, 110 = +10%, 75 = -25%.
    //
    // A value in the hundreds is not a tempo at all. GAYOBANG prints it on the
    // score ("♩=1400" over SOVIRGIN.GYB's opening notes) but plays nothing
    // there: the range is a silent lead-in that still takes its normal time,
    // and the song proper starts at the bar line afterwards. Treating it as a
    // tempo made the intro audible; skipping the ticks outright silenced it but
    // started the song too early and still let a crushed remnant through. So it
    // runs at the ordinary tempo with the output muted - see isMutedTick().
    int lastVal = 100;
    for (const auto& ee : m_globalEvents) {
        if ((uint64_t)ee.tick > tick) break;
        if (ee.value >= kIntroScaleThreshold) continue;   // marker, not a tempo
        lastVal = ee.value;
    }
    if (lastVal < 1) lastVal = 100;
    return lastVal;
}

unsigned long GybBackend::introSkipEndTick() const
{
    // Only an event sitting at tick 0 marks an intro; a large value later in a
    // song is a genuine tempo surge and must be played.
    if (m_globalEvents.isEmpty()) return 0;
    if (m_globalEvents.first().tick != 0) return 0;
    if (m_globalEvents.first().value < kIntroScaleThreshold) return 0;

    for (const auto& ee : m_globalEvents) {
        if (ee.value < kIntroScaleThreshold)
            return (unsigned long)ee.tick;
    }
    return 0;   // never returns to a normal tempo - play it rather than mute all
}

float GybBackend::tickHzAt(uint64_t tick) const
{
    // 0x34번지(m_effectiveBpmX100)는 실제 곡의 기본 템포(BPM * 100) 정보 필드입니다.
    // 글로벌 템포 스케일(scale)은 100% 기준의 템포 제어 배율이므로 실시간 BPM은 다음과 같이 정의됩니다:
    //   BPM = (m_effectiveBpmX100 / 100.0) * (scale / 100.0) * (user_scale / 100.0)
    //
    // A large value is NOT an absolute tempo - it is simply a large percentage.
    // This used to read scale >= 1000 as "absolute BPM x 10", so SOVIRGIN.GYB's
    // opening marker (value 1000) played at 100 BPM instead of 1000% of 140 =
    // 1400, and its intro flourish crawled past audibly for a second or two
    // before the song proper began. GAYOBANG shows the two markings as
    // "♩=1400" then "♩=140" on its score, which settles it: percentage
    // throughout, no special case (2026-08-10).
    int scale = globalTempoScale(tick);
    double baseBpm = ((double)m_effectiveBpmX100 / 100.0) * ((double)scale / 100.0);
    double currentBpm = baseBpm * ((double)m_userTempoScale / 100.0);

    // 가요방 하드웨어의 순수 타이머 재생 속도 공식: Hz = BPM * tbDiv / 60.0
    float rate = (float)(currentBpm * (double)m_tbDiv / 60.0);
    if (rate < 1.0f) rate = 7.0f;
    return rate;
}

int GybBackend::getCurrentBpm() const
{
    // Same percentage rule as tickHzAt - see the note there.
    int scale = globalTempoScale(m_currentTick);
    double baseBpm = ((double)m_effectiveBpmX100 / 100.0) * ((double)scale / 100.0);
    double currentBpm = baseBpm * ((double)m_userTempoScale / 100.0);
    return (int)qRound(currentBpm);
}

// No bank for .GYB instruments.
//
// A long detour on 2026-08-11 had this resolving every slot name in
// STANDARD.BNK and overwriting the song's table, on the strength of
// GAYOBANG.c:29616. Deleting the bank from a DOS GAYOBANG install and playing
// the library there showed that reading was wrong - everything played normally
// - and resolving names changed instruments in 88% of songs here. Only records
// the file leaves empty are filled from a bank; see fillEmptyInstrumentSlots().

// 0xC0 comes from the MODULATOR - settled from the decompiler, 2026-08-11.
//
// A long run of edits here took the feedback/connection byte from the carrier
// operator instead, on the reading that GAYOBANG's per-operator routine writes
// 0xC0 twice and the carrier lands last. It does not. FUN_255a_097c guards the
// write with DAT_445a_1363[op] == 0, and the same array appears in the 0x40
// writer deciding whether channel volume scales that operator - which in an FM
// connection is true only for the carrier. So the flag means "is carrier", and
// 0xC0 is written for the MODULATOR only. That is exactly AdPlug's
// send_operator(): modulator.fbc.
//
// The carrier's feed_back field is filler. 5,268 of the bank's 6,009 carrier
// bytes fall outside the valid 0..7 feedback range, and the value 47 - folding
// to feedback 7, the maximum - sits on most drum names where the modulator asks
// for 0. Routing it to 0xC0 put 99 of 368 GYB-folder slots at maximum feedback
// (the hissing edge) and gave the bass drum feedback 6 under SOVIRGIN's closing
// chord (the boom). None of it was ever right.
//
// It also never affected the bell it was introduced for: BELLS carries
// modulator fb = 4 and carrier fb = 4, so both routes write 0xC0 = 0x08.

void GybBackend::FixRhythmFrequency(int voice)
{
    // The two shared rhythm channels sit an octave lower than AdPlug puts them.
    //
    // In rhythm mode OPL channel 7 carries the snare and hi-hat and channel 8
    // the tom and cymbal, so one frequency serves two drums each. GAYOBANG sets
    // them once, in FUN_255a_1222, storing note 31 for channel 7 and note 24 for
    // channel 8 and running them through its own note table. Decoding that table
    // out of GAYOBANG.EXE - a word per semitone, where a value with the top bit
    // set means "the F-number for the next block up" - gives fnum 517 block 2 =
    // 98.0 Hz for channel 7 and fnum 690 block 1 = 65.4 Hz for channel 8.
    //
    // AdPlug instead feeds its own kSnareNote / kTomTomNote through the melodic
    // note path, which lands on 195.3 Hz and 130.1 Hz - exactly double, both of
    // them. Against a real-chip capture of SOVIRGIN that was the whole remaining
    // error: with drums muted our render sat 0.78 dB RMS from the chip across
    // 160 Hz-5 kHz, and with them it ran +6.35 dB at 80-160 Hz, all of it the
    // hi-hat and tom putting tonal energy where the chip has none.
    //
    // GAYOBANG never rewrites these after setup - a drum note-on only touches
    // the 0xBD key bits - but AdPlug's percussive NoteOn rewrites the channel
    // frequency every time, so this has to follow each one.
    if (!m_rhythmMode || voice < 7 || voice > 10) return;
    opl->write(0xA7, 0x05); opl->write(0xB7, 0x0A);   // fnum 517, block 2
    opl->write(0xA8, 0xB2); opl->write(0xB8, 0x06);   // fnum 690, block 1
}

void GybBackend::SetChannelVolume(int voice, uint8_t volume)
{
    // Channel volume, GAYOBANG's way: on an ADDITIVE instrument the modulator
    // is heard directly, so it is attenuated along with the carrier.
    //
    // FUN_255a_082e scales an operator's level when it is the carrier, OR when
    // that operator's own fm_type is 0 (the connection bit, additive), OR when
    // it is a rhythm drum operator. AdPlug's SetVolume only ever touches the
    // carrier - correct for an FM patch, where the modulator only bends the
    // carrier and its level is timbre rather than loudness, but wrong for an
    // additive one, where half the sound then ignores the channel volume.
    //
    // 133 of the 403 embedded instruments in the GYB library (33%) are
    // additive, which is why turning a channel down never moved it as far as
    // the DOS player did and the mix sat differently across channels.
    SetVolume(voice, volume);

    if (voice < 0 || voice >= 18) return;
    if (m_rhythmMode && voice >= 7) return;   // one operator each; SetVolume did it
    if (voice >= 9) return;
    if (!m_voiceAdditive[voice]) return;

    // Same arithmetic as GetKSLTL, so the two operators track each other. The
    // original divides by 128 rather than 127; at most one 0.75 dB step apart.
    static const int kOpTable[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };
    const uint8_t ksltl = m_voiceModKsltl[voice];
    uint16_t level = 63 - (ksltl & 0x3F);
    level = (uint16_t)(volume * level);
    level += level + 127;
    level = 63 - (level / 254);
    opl->write(0x40 + kOpTable[voice], (int)((ksltl & 0xC0) | level));
}

void GybBackend::NoteInstrument(int voice, int slot, int instIdx)
{
    // send_operator() writes the modulator's level straight from the patch, so
    // remember what it wrote and re-apply the channel volume over it.
    SetInstrument(voice, instIdx);
    if (voice < 0 || voice >= 18) return;
    m_voiceAdditive[voice] = false;
    m_voiceModKsltl[voice] = 0;
    if (slot >= 0 && slot < m_instParams.size()) {
        const QByteArray& p = m_instParams[slot];
        if (p.size() >= kEmbeddedParamLen) {
            // 13-byte operator record; [0] key_scale_level, [8] output_level,
            // [12] fm_type (0 = additive). See AdPlug read_fm_operator().
            m_voiceModKsltl[voice] = (uint8_t)(((uint8_t)p[0] << 6) | ((uint8_t)p[8] & 0x3F));
            m_voiceAdditive[voice] = ((uint8_t)p[12] == 0);
        }
    }
}

bool GybBackend::loadEmbeddedPatches()
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
        // mode / voice_number are not in the record; load_instrument_data()
        // zeroes them, which is what the BNK path produced for melodic
        // instruments too. Percussion is decided by the channel layout, not by
        // the instrument, so nothing is lost.
        //
        // Waveform is two bits. GAYOBANG's per-operator loader ends with
        // `record[13] = waveform & 3` (FUN_255a_06ad), and AdPlug writes the
        // byte from the record as it stands. 53 of the library's 403 slots
        // carry a waveform byte outside 0..3 - filler in the half of the record
        // a single-operator drum never uses, mostly, but not only. The chip
        // decodes three bits once OPL3 is enabled, which this player does, so a
        // byte with bit 2 set picked an OPL3-only waveform the original could
        // never have selected.
        QByteArray p2 = params;
        p2[26] = (char)(p2[26] & 3);
        p2[27] = (char)(p2[27] & 3);
        const int idx = load_instrument_data(
            reinterpret_cast<uint8_t*>(const_cast<char*>(p2.constData())),
            kEmbeddedParamLen);
        m_slotToInstIndex.append(idx);
        if (idx >= 0) ++loaded;
    }

    // Shown in the channel monitor. Keep the bank's name when GAYOBANG's bank
    // supplied the instruments, so it is visible which source is in play.
    if (m_bankName.isEmpty()) m_bankName = "embedded bank";
    qDebug() << "[GybBackend] embedded instruments:" << loaded << "/"
             << m_slotNames.size();
    return loaded > 0;
}

bool GybBackend::resolveBnkPatches(const CFileProvider& fp,
                                   const std::string& songFilename)
{
    // BNK lookup priority (mirrors ImsFileProvider behaviour):
    //   1. <song_dir>/<song_basename>.BNK (per-song bank, if it exists)
    //   2. app_dir/STANDARD.BNK
    //   3. app_dir/dos/STANDARD.BNK
    //   4. hardcoded dev path
    // External-bank override is handled by the Qt wrapper before load().

    QString songPath = QString::fromStdString(songFilename);
    QFileInfo songInfo(songPath);
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    if (!m_externalBankPath.isEmpty())
        candidates << m_externalBankPath;      // the user's explicit choice wins
    candidates << songInfo.absolutePath() + "/" + songInfo.completeBaseName() + ".BNK"
               << songInfo.absolutePath() + "/" + songInfo.completeBaseName() + ".bnk"
               << appDir + "/STANDARD.BNK"
               << appDir + "/dos/STANDARD.BNK"
               << "D:/py/midi-k-c260415/dos/STANDARD.BNK";

    SBnkHeader header;
    binistream* bnkFile = nullptr;
    QString chosen;
    for (const QString& c : candidates) {
        if (!QFile::exists(c)) continue;
        QByteArray cBytes = QFile::encodeName(c);
        bnkFile = fp.open(cBytes.constData());
        if (bnkFile) { chosen = c; break; }
    }
    if (!bnkFile) {
        qWarning() << "[GybBackend] No BNK found for" << songPath;
        return false;
    }

    if (!load_bnk_info(bnkFile, header)) {
        qWarning() << "[GybBackend] load_bnk_info failed:" << chosen;
        fp.close(bnkFile);
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
    qDebug() << "[GybBackend] BNK loaded:" << chosen
             << "slots=" << m_slotNames.size() << "hit=" << hit;
    return true;
}

void GybBackend::parseHeaderAndChannels()
{
    m_tracks.clear();
    m_globalEvents.clear();
    m_slotNames.clear();
    m_totalSongTicks = 0;

    const int fs = m_rawData.size();
    if (fs < 0x40) return;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(m_rawData.constData());

    int magic = d[0];
    int payload = (magic == 0x03) ? 0x40 : 0x4F;

    // Header tempo fields
    // 0x34번지는 BPM * 100을 담은 4바이트 uint32 필드입니다.
    m_effectiveBpmX100 = (fs >= 0x38) ? (d[0x34] | (d[0x35] << 8) | (d[0x36] << 16) | (d[0x37] << 24)) : 10000;
    if (m_effectiveBpmX100 < 3000 || m_effectiveBpmX100 > 30000) m_effectiveBpmX100 = 10000;
    
    // m_basicTempo는 0x32의 값이 원래 100 부근으로 깨져서 들어있으므로 사용하지 않고, 0x34에서 복원한 실제 BPM 값을 기준으로 씁니다.
    m_basicTempo = m_effectiveBpmX100 / 100;

    m_tbDiv = (fs > 0x28) ? d[0x28] : 4;
    if (m_tbDiv < 1 || m_tbDiv > 64) m_tbDiv = 4;

    // Header byte 0x2E = key transpose offset (default 60). The note value the
    // GYB note stream emits is `cmd`, GAYOBANG does `note = cmd - hdr[0x2E] + 60`
    // so the effective transpose to add to cmd is `60 - hdr[0x2E]`.
    int x2E = (fs > 0x2E) ? d[0x2E] : 60;
    m_keyTranspose = 60 - x2E;

    // Header byte 0x2D = rhythm mode. GAYOBANG keeps the song header at 0x8d2c
    // and passes 0x8d59 - that byte - to FUN_255a_1222, which sets the OPL
    // rhythm bit and the channel count (11 with it, 9 without). Neighbouring
    // fields in the same block pin the base address: 0x8d5a holds 60, the key
    // centre we already read at 0x2E, and 0x8d5e holds 100, the tempo at 0x32.
    m_rhythmMode = (fs > 0x2D) ? (d[0x2D] != 0) : true;

    // Globals
    int pos = payload;
    if (pos + 2 > fs) return;
    int gc = d[pos] | (d[pos+1] << 8); pos += 2;
    if (gc > 1000) gc = 0;
    for (int i = 0; i < gc; ++i) {
        if (pos + 3 >= fs) break;
        EventEntry e;
        e.tick = d[pos] | (d[pos+1] << 8);
        e.value = d[pos+2] | (d[pos+3] << 8);
        m_globalEvents.append(e);
        pos += 4;
    }

    // Per-channel blocks (0..10)
    for (int ch = 0; ch < 11; ++ch) {
        if (pos + 3 >= fs) break;
        if (d[pos] != ch) break;
        pos += 1;
        int endTick = d[pos] | (d[pos+1] << 8); pos += 2;
        int notestart = pos;
        int total = 0;
        while (total < endTick && pos + 1 < fs) {
            total += d[pos+1]; pos += 2;
        }
        int noteend = pos;
        int pc = d[pos] | (d[pos+1] << 8); pos += 2;
        int pcOff = pos; pos += pc * 4;
        int vc = d[pos] | (d[pos+1] << 8); pos += 2;
        int vOff = pos;  pos += vc * 4;
        int ptc = d[pos] | (d[pos+1] << 8); pos += 2;
        int ptOff = pos; pos += ptc * 4;

        TrackState t;
        t.channelId = ch;
        t.oplVoice  = ch;
        t.curPos    = notestart;
        t.noteEnd   = noteend;
        t.waitTicks = 0;
        t.activeNote = -1;
        t.done = false;
        t.program = 0;
        t.volume = 127;
        t.pitchBend = 0x2000;
        t.progIdx = t.volIdx = t.pitchIdx = 0;

        auto readEvents = [&](int off, int count, QList<EventEntry>& out) {
            for (int i = 0; i < count; ++i) {
                int p = off + i * 4;
                if (p + 3 >= fs) break;
                EventEntry e;
                e.tick = d[p] | (d[p+1] << 8);
                e.value = d[p+2];
                out.append(e);
            }
        };
        readEvents(pcOff, pc,  t.progEvents);
        readEvents(vOff,  vc,  t.volEvents);
        readEvents(ptOff, ptc, t.pitchEvents);

        if (total > m_totalSongTicks) m_totalSongTicks = total;
        if (endTick > m_totalSongTicks) m_totalSongTicks = endTick;
        m_tracks.append(t);
    }

    // Embedded instrument table: 38B records = 9B name + 1 unknown + 28B of OPL
    // parameters. Keep the parameters as well as the name - they are the actual
    // instrument (see loadEmbeddedPatches).
    int instCount = (fs > 0x3D) ? (d[0x3C] | (d[0x3D] << 8)) : 13;
    if (instCount <= 0 || instCount > 64) instCount = 13;
    m_instParams.clear();
    for (int i = 0; i < instCount; ++i) {
        int off = pos + i * 38;
        if (off + 38 > fs) break;
        QByteArray nameRaw(reinterpret_cast<const char*>(d + off), 9);
        int nul = nameRaw.indexOf('\0');
        if (nul >= 0) nameRaw.truncate(nul);
        m_slotNames.append(QString::fromLocal8Bit(nameRaw).trimmed());
        m_instParams.append(QByteArray(
            reinterpret_cast<const char*>(d + off + 10), kEmbeddedParamLen));
    }

    // Pre-scan note streams for the piano-roll. We walk each channel's
    // (cmd, dur) pairs and emit one RollNote per note-on (cmd 0x01..0x78).
    // cmd >= 0x79 is sustain (keeps current note ringing) so it doesn't
    // create a new roll entry.
    //
    // Display pitch = the actual SOUNDING MIDI note. The OPL path is:
    //   processChunk passes  note = cmd + m_keyTranspose  to NoteOn(), which
    //   does SetNote(note + kSilenceNote) with kSilenceNote = -12, i.e.
    //   biased_note = note - 12. The OPL frequency for biased_note converts
    //   back to MIDI = biased_note + 12.4 ≈ note. So the sounding MIDI note
    //   equals cmd + m_keyTranspose. (The old code added +60 here, pushing
    //   every note 5 octaves too high — off the right edge of the keyboard.)
    m_rollNotes.clear();
    for (const TrackState& t : m_tracks) {
        uint64_t curTick = 0;
        int pos2 = t.curPos;          // = notestart for this channel
        while (pos2 + 1 < t.noteEnd) {
            uint8_t cmd = d[pos2];
            uint8_t dur = d[pos2 + 1];
            if (cmd > 0 && cmd < 0x79) {
                RollNote rn;
                rn.tick = curTick;
                rn.channel = t.oplVoice;
                rn.pitch = (int)cmd + m_keyTranspose; // sounding MIDI note
                m_rollNotes.append(rn);
            }
            curTick += dur;
            pos2 += 2;
        }
    }

    std::sort(m_rollNotes.begin(), m_rollNotes.end(), [](const RollNote& a, const RollNote& b) {
        return a.tick < b.tick;
    });
}

bool GybBackend::load(const std::string& filename, const CFileProvider& fp)
{
    // Load whole file into memory. AdPlug's CFileProvider gives us a binistream
    // but we want byte-level random access, so just read everything.
    QString qf = QString::fromStdString(filename);
    QFile f(qf);
    if (!f.open(QIODevice::ReadOnly)) return false;
    m_rawData = f.readAll();
    f.close();
    if (m_rawData.size() < 0x40) return false;
    int magic = (uint8_t)m_rawData[0];
    if (magic != 0x03 && magic != 0x04) return false;

    m_songFilename = QFileInfo(qf).fileName().toLower();

    parseHeaderAndChannels();
    m_title = GybFileHandler::extractTitle(qf);
    if (m_title.isEmpty()) m_title = GybFileHandler::extractTitleFromLst(qf);
    if (m_title.isEmpty()) m_title = QFileInfo(qf).fileName();

    // .ROL-converted .GYB files carry instrument names with no parameters, the
    // same as their .OKA cousins - 32 of 334 songs in the library, 237 slots
    // that a program change actually selects. Those would play silently, so
    // resolve just those from a bank. Everything else stays untouched.
    // The song's own instruments. No bank: a DOS GAYOBANG with its bank deleted
    // plays the whole library normally, and resolving names from one changed
    // instruments across 88% of songs. Only records the file leaves empty -
    // .ROL conversions, 32 of 334 songs - are filled from a bank.
    fillEmptyInstrumentSlots(m_slotNames, m_instParams, qf,
                             kEmbeddedParamLen, "[GybBackend]");

    if (!loadEmbeddedPatches()) {
        if (!resolveBnkPatches(fp, filename)) {
            // Nothing usable — leave the map empty so every program falls back
            // to the AdPlug default instrument. Better than silence.
            m_slotToInstIndex.clear();
            for (int i = 0; i < m_slotNames.size(); ++i) m_slotToInstIndex.append(-1);
        }
    }

    rewind(0);
    return true;
}

void GybBackend::rewind(int subsong)
{
    // Let AdPlug reset OPL state and caches.
    CcomposerBackend::rewind(subsong);

    // Rhythm mode — GYB's channel layout (ch6=bdrum, ch7=snare, ch8=tom,
    // ch9=cymbal, ch10=hihat) matches AdPlug's percussive voice numbering when
    // rhythm mode is on. Without it the drum patches play through melodic
    // voices with the wrong timbre.
    //
    // It is a per-song flag, not a constant: FUN_255a_1222 takes it as an
    // argument and the caller passes the song header's byte, which also picks
    // 11 channels over 9. This was hardcoded on, so a song that asks for nine
    // melodic voices got OPL percussion instead - one file in the library
    // (WITHLOVE.GYB) carries 0 here.
    if (m_rhythmMode) SetRhythmMode(1);

    // NOTE-SEL on. The same routine writes register 0x08 = 0x40 while setting
    // up a song; AdPlug's init leaves it clear. It selects which F-number bit
    // joins the block in the key-scale number, which shifts every operator's
    // envelope rate scaling.
    opl->write(0x08, 0x40);

    // Restore per-track state.
    for (TrackState& t : m_tracks) {
        t.curPos = 0; // re-pointed below by parseHeaderAndChannels-emitted offsets
    }
    // Re-parse to reset note streams (cheap, and avoids tracking save/restore).
    parseHeaderAndChannels();
    m_currentTick = 0;
    m_tickHz = tickHzAt(0);

    // Pre-program each voice with its initial instrument (first PC event value
    // if any, otherwise slot 0). This makes the OPL voice ready before the
    // first NoteOn so the first note's envelope sounds clean.
    for (int i = 0; i < 18; ++i) {
        m_voiceSlot[i] = -1; m_voiceAttack[i] = 0;
        m_voiceAdditive[i] = false; m_voiceModKsltl[i] = 0;
    }
    for (TrackState& t : m_tracks) {
        int prog = 0;
        if (!t.progEvents.isEmpty()) prog = t.progEvents.front().value;
        t.program = prog;
        if (prog >= 0 && prog < m_slotToInstIndex.size()) {
            int instIdx = m_slotToInstIndex[prog];
            if (instIdx >= 0) NoteInstrument(t.oplVoice, prog, instIdx);
        }
        if (t.oplVoice >= 0 && t.oplVoice < 18) m_voiceSlot[t.oplVoice] = prog;
        SetChannelVolume(t.oplVoice, t.volume);
        ChangePitch(t.oplVoice, t.pitchBend);
    }
}

bool GybBackend::update()
{
    // Process the current tick, then update m_tickHz so that getrefresh()
    // returned to the caller AFTER update() reflects the rate at the NEXT tick.
    // This makes the standard player loop pattern (`refresh = getrefresh();
    // samples = sampleRate / refresh; update();`) correctly honour mid-song
    // tempo changes — previously the rate lagged by a tick and slow segments
    // (e.g. POET / LOVEIS end-of-song slow-down) bloated the duration.
    bool alive = advanceOneTick();
    m_tickHz = tickHzAt(m_currentTick);
    return alive;
}

bool GybBackend::advanceOneTick()
{
    bool anyAlive = false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(m_rawData.constData());
    const int fs = m_rawData.size();
    int curTick = (int)m_currentTick;

    for (TrackState& t : m_tracks) {
        if (t.done) continue;

        applyChannelEvents(t, curTick);

        if (t.waitTicks > 0) {
            t.waitTicks--;
            anyAlive = true;
            continue;
        }
        while (t.waitTicks == 0 && !t.done && t.curPos + 1 < t.noteEnd) {
            uint8_t cmd = d[t.curPos];
            uint8_t dur = d[t.curPos + 1];
            t.curPos += 2;
            t.waitTicks = processChunk(t, cmd, dur);
            if (t.waitTicks == 0) break;
        }
        if (t.curPos + 1 >= t.noteEnd && t.waitTicks == 0) {
            // Key the last note off when a track's data runs out.
            //
            // Four policies were rendered offline and scored against the DOSBox
            // capture, aligned on the closing bell strike (centroid + decay):
            //
            //   key off everything (this)          70   <- original
            //   drums keep ringing                 51
            //   key off nothing (what .OKA does)   50
            //   melodic keeps ringing, drums off   59
            //
            // Every one of them is far from DOS, and the spread between them is
            // small, so the note-off policy is NOT what makes our ending differ.
            // The measured gap is that DOS's ending stays ~5 dB louder and runs
            // ~1.5 s longer than ours whatever we do here. Left at the original
            // behaviour until the real cause is found; do not re-litigate this
            // without re-running scratchpad/opltrace.
            // Key the last note off when a track's data runs out.
            //
            // Removing this was tried twice on 2026-08-11 and is wrong both
            // times. The reasoning looked sound - SOVIRGIN's BELLS has modulator
            // release 0 against carrier release 2, so a key-off freezes the
            // modulator while the carrier fades and the bell decays into its own
            // 2540 Hz sideband, which a register-stream diff against the same
            // song as .OKA (a format with no note-offs at all) pinned down. But
            // holding the note instead turns the closing bell into a swept
            // electronic tone, which is worse. Whatever the sideband tail is, it
            // is not fixed here.
            t.done = true;
            if (t.activeNote >= 0) { NoteOff(t.oplVoice); t.activeNote = -1; }
        } else {
            anyAlive = true;
            if (t.waitTicks > 0) t.waitTicks--;
        }
    }
    ++m_currentTick;
    return anyAlive;
}

int GybBackend::processChunk(TrackState& t, uint8_t cmd, uint8_t dur)
{
    if (cmd == 0) {
        // Rest — silence the voice.
        if (t.activeNote >= 0) { NoteOff(t.oplVoice); t.activeNote = -1; }
    } else if (cmd < 0x79) {
        // Note On. AdPlug's NoteOn adds kSilenceNote (=60) internally, so we
        // pass the raw cmd value with the song's key transpose subtracted —
        // mirroring GAYOBANG's `note = cmd - hdr[0x2E] + 60` (the +60 is done
        // inside AdPlug) plus user transpose.
        int note = (int)cmd + m_keyTranspose + m_userKeyTranspose;
        if (note < 0) note = 0;
        if (note > 0x7F) note = 0x7F;
        if (t.activeNote >= 0) NoteOff(t.oplVoice);
        NoteOn(t.oplVoice, note);
        FixRhythmFrequency(t.oplVoice);
        t.activeNote = cmd;
        // Record the onset for the level meter (peak = current channel volume),
        // so the bar jumps on each note and decays between — like the OKA meter.
        if (t.oplVoice >= 0 && t.oplVoice < 18) {
            int lvl = t.volume; if (lvl < 1) lvl = 1; if (lvl > 127) lvl = 127;
            if (lvl > m_voiceAttack[t.oplVoice]) m_voiceAttack[t.oplVoice] = lvl;
        }
    }
    // cmd >= 0x79 → sustain (keep current note ringing, no OPL writes).
    return (int)dur;
}

void GybBackend::applyChannelEvents(TrackState& t, int currentTick)
{
    // Program change events
    while (t.progIdx < t.progEvents.size() &&
           t.progEvents[t.progIdx].tick <= currentTick) {
        int newProg = t.progEvents[t.progIdx].value;
        if (newProg != t.program) {
            t.program = newProg;
            if (newProg >= 0 && newProg < m_slotToInstIndex.size()) {
                int instIdx = m_slotToInstIndex[newProg];
                if (instIdx >= 0) {
                    // Load the instrument; do NOT re-strike a sounding note.
                    //
                    // This used to key the held note off, load, and key it on
                    // again, which turns every mid-note program change into an
                    // extra hit. That is the second sound behind SOVIRGIN's
                    // closing bell: its drum channel changes instrument at tick
                    // 3008, on the same tick as the bell, and the re-strike
                    // fires the bass drum (fnum 0x157 / block 2 = 65 Hz) at
                    // full volume over it. Measured on the last second of the
                    // render, the 65 Hz band rose 588 -> 133 while every other
                    // band decayed; without the re-strike it decays too
                    // (118 -> 15), matching the .OKA of the same song, which
                    // has no such hit. Across the GYB folder 18 of 25 songs
                    // change at all and none by more than 0.54% RMS - the
                    // re-strike was adding stray hits, not carrying the music.
                    //
                    // Changing an instrument mid-note is what the chip does
                    // anyway: the operator registers change and the running
                    // envelope continues. SetInstrument re-applies the channel
                    // volume itself, so nothing else is needed here.
                    NoteInstrument(t.oplVoice, newProg, instIdx);
                    SetChannelVolume(t.oplVoice, (uint8_t)t.volume);
                }
            }
            // Keep the channel monitor's voice→slot map current.
            if (t.oplVoice >= 0 && t.oplVoice < 18) m_voiceSlot[t.oplVoice] = newProg;
        }
        t.progIdx++;
    }
    // Volume events — file byte 0..100 → scale to 0..127.
    while (t.volIdx < t.volEvents.size() &&
           t.volEvents[t.volIdx].tick <= currentTick) {
        int raw = t.volEvents[t.volIdx].value & 0xFF;
        int vol = (raw * 0x7F) / 100;
        if (vol < 0) vol = 0;
        if (vol > 127) vol = 127;
        t.volume = vol;
        SetChannelVolume(t.oplVoice, (uint8_t)vol);
        t.volIdx++;
    }
    // Pitch events — byte × 0x333 centered around 0x2000, val=10 snap to neutral.
    while (t.pitchIdx < t.pitchEvents.size() &&
           t.pitchEvents[t.pitchIdx].tick <= currentTick) {
        int raw = t.pitchEvents[t.pitchIdx].value & 0xFF;
        int bend = (raw == 10) ? 0x2000 : raw * 0x333;
        if (bend > 0x3FFF) bend = 0x3FFF;
        if (bend != t.pitchBend) {
            t.pitchBend = bend;
            ChangePitch(t.oplVoice, (uint16_t)bend);
        }
        t.pitchIdx++;
    }
}

std::string GybBackend::getvoiceinstrument(unsigned int voice)
{
    if (voice >= 18) return std::string();
    int slot = m_voiceSlot[voice];
    if (slot >= 0 && slot < m_slotNames.size()) {
        return m_slotNames[slot].toStdString();
    }
    return std::string();
}
