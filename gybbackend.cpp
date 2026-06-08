#include "gybbackend.h"
#include "gybfilehandler.h"
#include <QFile>
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
    // Global events are tempo-scale percentages applied to header[0x34].
    // value 100 = normal, 110 = +10%, 75 = -25%. 1000-class values are
    // intro-skip markers — pass through so the affected tick range
    // collapses in wall-clock time.
    int lastVal = 100;
    for (const auto& ee : m_globalEvents) {
        if ((uint64_t)ee.tick > tick) break;
        lastVal = ee.value;
    }
    if (lastVal < 1) lastVal = 100;
    return lastVal;
}

float GybBackend::tickHzAt(uint64_t tick) const
{
    // 0x34번지(m_effectiveBpmX100)는 실제 곡의 기본 템포(BPM * 100) 정보 필드입니다.
    // 글로벌 템포 스케일(scale)은 100% 기준의 템포 제어 배율이므로 실시간 BPM은 다음과 같이 정의됩니다:
    //   BPM = (m_effectiveBpmX100 / 100.0) * (scale / 100.0) * (user_scale / 100.0)
    // 단, scale이 1000 이상인 경우 이는 absolute BPM * 10으로 해석합니다. (예: 1000 = 100.0 BPM)
    int scale = globalTempoScale(tick);
    double baseBpm;
    if (scale >= 1000) {
        baseBpm = (double)scale / 10.0;
    } else {
        baseBpm = ((double)m_effectiveBpmX100 / 100.0) * ((double)scale / 100.0);
    }
    double currentBpm = baseBpm * ((double)m_userTempoScale / 100.0);

    // 가요방 하드웨어의 순수 타이머 재생 속도 공식: Hz = BPM * tbDiv / 60.0
    float rate = (float)(currentBpm * (double)m_tbDiv / 60.0);
    if (rate < 1.0f) rate = 7.0f;
    return rate;
}

int GybBackend::getCurrentBpm() const
{
    int scale = globalTempoScale(m_currentTick);
    double baseBpm;
    if (scale >= 1000) {
        baseBpm = (double)scale / 10.0;
    } else {
        baseBpm = ((double)m_effectiveBpmX100 / 100.0) * ((double)scale / 100.0);
    }
    double currentBpm = baseBpm * ((double)m_userTempoScale / 100.0);
    return (int)qRound(currentBpm);
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

    // Embedded instrument slot names (38B records: 9B name + 29B params).
    int instCount = (fs > 0x3D) ? (d[0x3C] | (d[0x3D] << 8)) : 13;
    if (instCount <= 0 || instCount > 64) instCount = 13;
    for (int i = 0; i < instCount; ++i) {
        int off = pos + i * 38;
        if (off + 9 > fs) break;
        QByteArray nameRaw(reinterpret_cast<const char*>(d + off), 9);
        int nul = nameRaw.indexOf('\0');
        if (nul >= 0) nameRaw.truncate(nul);
        m_slotNames.append(QString::fromLocal8Bit(nameRaw).trimmed());
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

    if (!resolveBnkPatches(fp, filename)) {
        // No BNK — leave m_slotToInstIndex empty so all programs fall back to
        // the AdPlug default instrument. Better than nothing.
        m_slotToInstIndex.clear();
        for (int i = 0; i < m_slotNames.size(); ++i) m_slotToInstIndex.append(-1);
    }

    rewind(0);
    return true;
}

void GybBackend::rewind(int subsong)
{
    // Let AdPlug reset OPL state and caches.
    CcomposerBackend::rewind(subsong);

    // Enable rhythm mode — GYB's channel layout (ch6=bdrum, ch7=snare, ch8=tom,
    // ch9=cymbal, ch10=hihat) matches AdPlug's percussive voice numbering when
    // rhythm mode is on. Without it the drum patches play through melodic
    // voices with the wrong timbre.
    if (m_rhythmMode) SetRhythmMode(1);

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
    for (int i = 0; i < 18; ++i) { m_voiceSlot[i] = -1; m_voiceAttack[i] = 0; }
    for (TrackState& t : m_tracks) {
        int prog = 0;
        if (!t.progEvents.isEmpty()) prog = t.progEvents.front().value;
        t.program = prog;
        if (prog >= 0 && prog < m_slotToInstIndex.size()) {
            int instIdx = m_slotToInstIndex[prog];
            if (instIdx >= 0) SetInstrument(t.oplVoice, instIdx);
        }
        if (t.oplVoice >= 0 && t.oplVoice < 18) m_voiceSlot[t.oplVoice] = prog;
        SetVolume(t.oplVoice, t.volume);
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
                    int saved = t.activeNote;
                    if (saved >= 0) NoteOff(t.oplVoice);
                    SetInstrument(t.oplVoice, instIdx);
                    if (saved >= 0) {
                        int note = saved + m_keyTranspose + m_userKeyTranspose;
                        if (note < 0) note = 0;
                        if (note > 0x7F) note = 0x7F;
                        NoteOn(t.oplVoice, note);
                    }
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
        SetVolume(t.oplVoice, (uint8_t)vol);
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
