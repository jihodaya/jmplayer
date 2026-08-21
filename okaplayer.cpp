#include "okaplayer.h"
#include "okabackend.h"
#include "okafilehandler.h"
#include <QDebug>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "jjomesynth.h"
#include "opltunnelsender.h" // OPL register tunnel to the bare-metal jukebox

// Rhythm-operator trim, 0.75 dB per step: 0 = off, 6 = -4.5 dB, 8 = -6 dB.
static const int kDrumTLOffset = 6;

class OkaPlayer::InterceptingOpl : public CNemuopl {
public:
    struct ShadowVoice {
        uint8_t ammulti_mod = 0;
        uint8_t ardr_mod = 0;
        uint8_t slrr_mod = 0;
        uint8_t wave_mod = 0;
        
        uint8_t ammulti_car = 0;
        uint8_t ardr_car = 0;
        uint8_t slrr_car = 0;
        uint8_t wave_car = 0;
    } m_shadowVoices[18];

    struct OpMapping {
        int voice;
        bool isCarrier;
    } m_opMapping[0x200];

    bool m_drumMode = false;

    InterceptingOpl(int rate, bool* keyOn, int* volume, int* regA, int* regB)
        : CNemuopl(rate), m_keyOn(keyOn), m_volume(volume), m_regA(regA), m_regB(regB) {
        
        for (int i = 0; i < 0x200; ++i) {
            m_opMapping[i] = { -1, false };
        }
        static const uint8_t op_table[9] = {
            0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12
        };
        for (int ch = 0; ch < 18; ++ch) {
            int bank = (ch >= 9) ? 0x100 : 0;
            int chIdx = ch % 9;
            int modOp = op_table[chIdx];
            int carOp = op_table[chIdx] + 3;
            
            m_opMapping[bank | modOp] = { ch, false };
            m_opMapping[bank | carOp] = { ch, true };
        }
        
        clearInternal();
    }

    void init() override {
        // OPL tunnel: tell the jukebox to reset+OPL3-enable its own chip. The
        // 512-write deep reset below stays LOCAL (redundant with the jukebox's
        // OPL3_Reset, and ~0.5s of serial at 31250) - same as ImsPlayer.
        OplTunnelSender::instance().reset();

        CNemuopl::init();
        for (int i = 0x00; i <= 0xFF; ++i) {
            CNemuopl::write(i, 0);
            CNemuopl::write(i | 0x100, 0);
        }
        CNemuopl::write(0x101, 0x20); // OPL3 Mode Enable
        CNemuopl::write(0x105, 0x01); // OPL3 Mode Enable (Bit 0 = 1, required for stereo)
        clearInternal();
    }

    // Local chip write + tunnel to the jukebox (no-op when the tunnel is off).
    // 9-bit tunnel register = (currChip<<8)|reg, same as CNemuopl::write forms.
    inline void tunnelChipWrite(int reg, int val) {
        CNemuopl::write(reg, val);
        OplTunnelSender::instance().queueWrite(
            (uint16_t)(((getchip() & 1) << 8) | (reg & 0x1FF)), (uint8_t)val);
    }
    void clearInternal() {
        memset(m_keyOn, 0, 18 * sizeof(bool));
        memset(m_volume, 0, 18 * sizeof(int));
        memset(m_regA, 0, 18 * sizeof(int));
        memset(m_regB, 0, 18 * sizeof(int));
        m_drumMode = false;
        for (int i = 0; i < 18; ++i) {
            originalPanReg[i] = 0x30; // Default to Center (Both)
            m_shadowVoices[i] = ShadowVoice();
        }
    }
    void write(int reg, int val) override {
        int bankOffset = (reg >= 0x100) ? 9 : 0;
        int baseReg    = reg & 0xFF;

        // Same rhythm-operator trim as GybPlayer -
        // see the note there. Measured against a real-chip capture, not derived
        // from NORE45.
        if (kDrumTLOffset && baseReg >= 0x50 && baseReg <= 0x55 && reg < 0x100) {
            int tl = (val & 0x3F) + kDrumTLOffset;
            if (tl > 63) tl = 63;
            val = (val & 0xC0) | tl;
        }

        // Waveform is two bits on the OPL2 these drivers were written for.
        // The record-level mask in the backends covers the song's own patches;
        // this catches every other route to 0xE0..0xF5 as well, so an OPL3-only
        // waveform can never be selected while OPL3 mode is on for the stereo
        // bits alone.
        if (baseReg >= 0xE0 && baseReg <= 0xF5) {
            val &= 0x03;
        }

        // OPL3 모드 끄기(OPL2 복원) 방지 필터링
        if (reg == 0x105) {
            val |= 0x01;
        }

        // OPL3 Panning (0xC0~0xC8) 가로채기 및 유사 스테레오 변조
        if (baseReg >= 0xC0 && baseReg <= 0xC8) {
            int ch = bankOffset + (baseReg - 0xC0);
            if (ch < 18) {
                originalPanReg[ch] = val; // Store original register value
                // OPL tunnel: ship the ORIGINAL (pre-pan) value - the jukebox
                // applies its own virtual-stereo policy (same as its file
                // playback). jmp's pan map stays local. See ImsPlayer's 0xC0
                // intercept for the full rationale.
                OplTunnelSender::instance().queueWrite(
                    (uint16_t)(((getchip() & 1) << 8) | (reg & 0x1FF)), (uint8_t)val);
                // Mode 10 does not use the pan map: the two chips are the
                // stereo. Set the first one explicitly, because the map's
                // letters are the other way round from the chip's bits -
                // nukedopl.c takes 0x10 as cha and mixes cha into the LEFT
                // output, while getChannelPanBit() returns 0x20 for 'L'.
                const bool dualChip = JJoMeSynth::instance().getOplStereoMode() == 10;
                int panBit = dualChip ? 0x20   // first chip -> right
                                      : JJoMeSynth::instance().getChannelPanBit(ch);
                val &= ~0x30;
                val |= panBit;
                CNemuopl::write(reg, val); // local chip only - jmp's own stereo
                if (dualChip) mirrorSecondChip(reg, val);   // second chip -> left
                return;
            }
        }

        // 섀도잉 레지스터 업데이트 (악기 추적용)
        int bank = (reg >= 0x100) ? 0x100 : 0;
        int opOffset = -1;
        int regGroup = 0;
        if (baseReg >= 0x20 && baseReg <= 0x35) { opOffset = baseReg - 0x20; regGroup = 0x20; }
        else if (baseReg >= 0x60 && baseReg <= 0x75) { opOffset = baseReg - 0x60; regGroup = 0x60; }
        else if (baseReg >= 0x80 && baseReg <= 0x95) { opOffset = baseReg - 0x80; regGroup = 0x80; }
        else if (baseReg >= 0xE0 && baseReg <= 0xF5) { opOffset = baseReg - 0xE0; regGroup = 0xE0; }
        
        if (opOffset >= 0) {
            auto map = m_opMapping[bank | opOffset];
            if (map.voice >= 0) {
                int ch = map.voice;
                if (map.isCarrier) {
                    if (regGroup == 0x20) m_shadowVoices[ch].ammulti_car = val;
                    else if (regGroup == 0x60) m_shadowVoices[ch].ardr_car = val;
                    else if (regGroup == 0x80) m_shadowVoices[ch].slrr_car = val;
                    else if (regGroup == 0xE0) m_shadowVoices[ch].wave_car = val;
                } else {
                    if (regGroup == 0x20) m_shadowVoices[ch].ammulti_mod = val;
                    else if (regGroup == 0x60) m_shadowVoices[ch].ardr_mod = val;
                    else if (regGroup == 0x80) m_shadowVoices[ch].slrr_mod = val;
                    else if (regGroup == 0xE0) m_shadowVoices[ch].wave_mod = val;
                }
            }
        }

        // 1. 드럼 모드 감시
        if (reg == 0xBD) {
            m_drumMode = (val & 0x20) != 0;
        }

        // OPL3 F-Number & Block 가로채기
        if (baseReg >= 0xA0 && baseReg <= 0xA8) {
            int ch = bankOffset + (baseReg - 0xA0);
            if (ch < 18) m_regA[ch] = val;
        } else if (baseReg >= 0xB0 && baseReg <= 0xB8) {
            int ch = bankOffset + (baseReg - 0xB0);
            if (ch < 18) m_regB[ch] = val;
        }

        if (reg < 0x100) m_shadow0[reg & 0xFF] = (uint8_t)val;
        tunnelChipWrite(reg, val);
        if (JJoMeSynth::instance().getOplStereoMode() == 10)
            mirrorSecondChip(reg, val);

        if (baseReg >= 0xB0 && baseReg <= 0xB8) {
            int ch = bankOffset + (baseReg - 0xB0);
            m_keyOn[ch] = (val & 0x20) != 0;
            if (!m_keyOn[ch]) m_volume[ch] = 0;
        } else if (reg == 0xBD) {
            bool drumMode = (val & 0x20) != 0;
            if (drumMode) {
                m_keyOn[6]  = (val & 0x10) != 0; // BD
                m_keyOn[7]  = (val & 0x08) != 0; // SD
                m_keyOn[8]  = (val & 0x04) != 0; // TOM
                m_keyOn[9]  = (val & 0x02) != 0; // CYM
                m_keyOn[10] = (val & 0x01) != 0; // HH
            }
        } else if (baseReg >= 0x40 && baseReg <= 0x55) {
            static const int opToCh[22] = {
                0, 1, 2, 0, 1, 2, -1, -1, 3, 4, 5, 3, 4, 5,
                -1, -1, 6, 7, 8, 6, 7, 8
            };
            int op = baseReg - 0x40;
            if (op < 22) {
                int ch = opToCh[op];
                if (ch != -1) {
                    int realCh = bankOffset + ch;
                    int tl = val & 0x3F;
                    int v  = ((63 - tl) * 127) / 63;
                    if (v > m_volume[realCh]) m_volume[realCh] = v;
                }
            }
            if (baseReg == 0x51) m_volume[7]  = ((63 - (val & 0x3F)) * 127) / 63;
            if (baseReg == 0x52) m_volume[10] = ((63 - (val & 0x3F)) * 127) / 63;
            if (baseReg == 0x55) m_volume[9]  = ((63 - (val & 0x3F)) * 127) / 63;
        }
    }

public:
    // Release every sounding voice without disturbing instrument setup, so a
    // paused song resumes cleanly (2026-07-27; see ImsPlayer's identical
    // silenceAllVoices - fixes the "칭~" swell on resume, locally and over the
    // mt32-pi OPL tunnel). Clears key-on bit 5 of B0-B8 (both banks) and the
    // 0xBD drum key bits; goes through write() so the tunnel carries it too.
    // GAYOBANG's "스테레오" / NORE45's equivalent, offered as mode [0].
    //
    // The Oksori card answers at 0x388 AND 0x38A - two OPL chips, one per
    // speaker - and the setting is not a pan layout: FUN_255a_12e9 turns the
    // second chip on, every operator register is mirrored to it, and
    // FUN_255a_0ccb gives it the same note with the pitch bend raised 0x600,
    // which is +18.75 cents at the 1-semitone range. Above sound-mode 3
    // (서라운드 / 회전 2) the mirrored level is 9/10; plain 스테레오 is full.
    // Rhythm is not mirrored - the gate there is `channel < 7 || !rhythmMode` -
    // so the drums come from the first chip only, which is why the two sides
    // do not sit at the same level.
    //
    // Measured against a capture of the real thing: L/R correlation 0.164
    // against the recording's 0.023, and a channel imbalance of +4.8 dB
    // against +5.1. A pan pattern cannot get there - the widest one managed
    // 0.101 and none of them carry the detune that makes it sound thick.
    uint8_t m_shadow0[256] = {0};

    // Bring the second chip up to date from the shadow, for when the mode is
    // switched while a song is already playing. Without it the chip has never
    // seen the operator or frequency registers of the notes now sounding, and
    // only whatever arrives next would reach it.
    // Key-off every voice on the mirror chip, and drop its rhythm bits. See the
    // same function in gybplayer.cpp for why leaving mode 10 needs this.
    void silenceSecondChip() {
        for (int ch = 0; ch < 9; ++ch) {
            const int regB = 0x1B0 + ch;
            CNemuopl::write(regB, m_shadow0[0xB0 + ch] & ~0x20);
        }
        CNemuopl::write(0x1BD, m_shadow0[0xBD] & ~0x1F);   // rhythm key-ons
    }

    void rebuildSecondChip() {
        for (int r = 0x20; r <= 0xF5; ++r) {
            const int b = r & 0xFF;
            const bool interesting =
                (b >= 0x20 && b <= 0x35) || (b >= 0x40 && b <= 0x55) ||
                (b >= 0x60 && b <= 0x75) || (b >= 0x80 && b <= 0x95) ||
                (b >= 0xE0 && b <= 0xF5) || (b >= 0xC0 && b <= 0xC8);
            if (interesting) mirrorSecondChip(r, m_shadow0[b]);
        }
        for (int ch = 0; ch < 9; ++ch) mirrorFreq(ch, m_drumMode ? 6 : 8);
    }

    void mirrorSecondChip(int reg, int val) {
        if (reg >= 0x100) return;
        const int b = reg & 0xFF;
        const int lastCh = m_drumMode ? 6 : 8;
        static const int kOp[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };
        auto chOfOp = [&](int op) {
            for (int c = 0; c < 9; ++c) if (kOp[c] == op || kOp[c] + 3 == op) return c;
            return -1;
        };
        if (b >= 0xA0 && b <= 0xA8) { mirrorFreq(b - 0xA0, lastCh); return; }
        if (b >= 0xB0 && b <= 0xB8) { mirrorFreq(b - 0xB0, lastCh); return; }
        if (b >= 0xC0 && b <= 0xC8) {
            if (b - 0xC0 <= lastCh) CNemuopl::write(b | 0x100, (val & 0x0F) | 0x10);
            return;
        }
        if ((b >= 0x20 && b <= 0x35) || (b >= 0x40 && b <= 0x55) ||
            (b >= 0x60 && b <= 0x75) || (b >= 0x80 && b <= 0x95) ||
            (b >= 0xE0 && b <= 0xF5)) {
            const int c = chOfOp(b & 0x1F);
            if (c >= 0 && c <= lastCh) CNemuopl::write(b | 0x100, val);
        }
    }

    void mirrorFreq(int ch, int lastCh) {
        if (ch < 0 || ch > lastCh) return;
        int fnum  = ((m_regB[ch] & 3) << 8) | (m_regA[ch] & 0xFF);
        int block = (m_regB[ch] >> 2) & 7;
        fnum = (int)(fnum * 1.0108920f + 0.5f);          // +0x600 of bend
        if (fnum > 0x3FF) { if (block < 7) { fnum >>= 1; ++block; } else fnum = 0x3FF; }
        CNemuopl::write(0x1A0 + ch, fnum & 0xFF);
        CNemuopl::write(0x1B0 + ch, (m_regB[ch] & 0x20) | (block << 2) | ((fnum >> 8) & 3));
    }

    void silenceAllVoices() {
        for (int ch = 0; ch < 18; ++ch) {
            const int bank = (ch >= 9) ? 0x100 : 0;
            write(bank + 0xB0 + (ch % 9), m_regB[ch] & ~0x20);
        }
        if (m_drumMode)
            write(0xBD, 0x20); // keep rhythm mode on, clear the 5 drum key bits
    }

    int originalPanReg[18];
private:
    bool* m_keyOn;
    int*  m_volume;
    int*  m_regA;
    int*  m_regB;
};

OkaPlayer::OkaPlayer(QObject *parent)
    : QObject(parent), m_opl(nullptr), m_backend(nullptr),
      m_playing(false), m_sampleRate(49716),
      m_position(0), m_sampleCounter(0.0f), m_volume(100),
      m_dspLevel(0), m_currentTick(0), m_duration(0),
      m_lpfLastL(0.0f), m_lpfLastR(0.0f),
      m_needsFadeIn(false), m_fadeCounter(0)
{
    for (int i = 0; i < 18; ++i) m_voiceLevelOut[i].store(0);
    for (int i = 0; i < 64; ++i) m_instLevelOut[i].store(0);
    memset(m_cachedVoiceInsts, 0, sizeof(m_cachedVoiceInsts));
    memset(m_cachedVoiceNotes, 0, sizeof(m_cachedVoiceNotes));
    memset(m_cachedVoiceVols, 0, sizeof(m_cachedVoiceVols));
    memset(m_cachedVoiceKeyOn, 0, sizeof(m_cachedVoiceKeyOn));
}

OkaPlayer::~OkaPlayer()
{
    stop();
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_backend) { delete m_backend; m_backend = nullptr; }
    if (m_opl)     { delete m_opl;     m_opl     = nullptr; }
}

bool OkaPlayer::loadFile(const QString& fileName)
{
    stop();

    if (!OkaFileHandler::isOkaFile(fileName)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_backend) { delete m_backend; m_backend = nullptr; }
    if (m_opl)     { delete m_opl;     m_opl     = nullptr; }

    m_opl     = new InterceptingOpl(m_sampleRate, m_oplKeyOn, m_oplVolume, m_oplRegA, m_oplRegB);
    m_backend = new OkaBackend(m_opl);
    if (!m_externalBankPath.isEmpty())
        m_backend->setExternalBankPath(m_externalBankPath);

    // toUtf8, not encodeName - see the matching note in GybPlayer::loadFile.
    // OkaBackend::load reads this back with QString::fromStdString (UTF-8).
    QByteArray fbytes = fileName.toUtf8();
    if (!m_backend->load(fbytes.constData())) {
        delete m_backend; m_backend = nullptr;
        delete m_opl;     m_opl     = nullptr;
        return false;
    }

    // Pre-scan to calculate wall-clock duration
    // OPL tunnel: this scan runs the WHOLE SONG through the REAL m_opl -
    // suppress the wire (see GybPlayer::loadFile), unsuppress after the final
    // rewind so the resync snapshot ships the clean song-start state.
    OplTunnelSender::instance().setSuppressed(true);
    m_backend->rewind(0);
    unsigned long totalTicks = 0;
    double totalMs = 0.0;
    while (totalTicks < 1000000) {
        float r = m_backend->getrefresh();
        if (r < 1.0f) r = 120.0f;
        totalMs += 1000.0 / (double)r;
        if (!m_backend->update()) break;
        ++totalTicks;
    }
    m_duration = (unsigned long)totalMs;
    m_backend->rewind(0);
    OplTunnelSender::instance().setSuppressed(false);

    m_title    = m_backend->title();
    m_bankName = m_backend->bankName();

    QStringList slotList = m_backend->slotNames();
    m_instrumentNames.clear();
    m_instrumentNames = slotList;

    // Generate roll notes for Piano Roll and collect lyric markers
    m_rollNotes.clear();
    m_lyricMarkerTicks.clear();
    const QList<OkaMidiEvent>& events = m_backend->getEvents();
    for (const auto& ev : events) {
        if (!ev.isMeta && (ev.status & 0xF0) == 0x90) {
            int channel = ev.status & 0x0F;
            int vel = ev.data2;
            if (vel > 0) {
                if (channel < 11) {          // 11 voices, see OkaBackend
                    OkaRollNote rn;
                    rn.tick = ev.absoluteTick;
                    rn.channel = channel;
                    rn.pitch = ev.data1;
                    m_rollNotes.append(rn);
                }
                // Channel 10 is the hi-hat, not a lyric marker (see
                // OkaBackend::advanceOneTick). It is still collected here
                // because it is the last-resort fallback when a file's
                // trailing lyric sync block is missing or corrupt.
                if (channel == 10) {
                    m_lyricMarkerTicks.append(ev.absoluteTick);
                }
            }
        }
    }

    m_currentTick.store(0);
    m_sampleCounter.store(0.0f);
    m_position.store(0);
    m_positionRemainder = 0.0;
    m_needsFadeIn.store(true);
    m_fadeCounter.store(0);
    memset(m_cachedVoiceInsts, 0, sizeof(m_cachedVoiceInsts));
    memset(m_cachedVoiceNotes, 0, sizeof(m_cachedVoiceNotes));
    memset(m_cachedVoiceVols, 0, sizeof(m_cachedVoiceVols));
    memset(m_cachedVoiceKeyOn, 0, sizeof(m_cachedVoiceKeyOn));


    return true;
}

void OkaPlayer::play()
{
    if (!m_backend || !m_opl) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (!m_playing.load()) {
        m_playing.store(true);
    }
}

void OkaPlayer::pause() {
    m_playing.store(false);
    // Release sounding voices so nothing swells back in on resume (see
    // InterceptingOpl::silenceAllVoices; fixes the "칭~" locally + over tunnel).
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_opl) m_opl->silenceAllVoices();
}

void OkaPlayer::stop()
{
    // Repeated STOP while already stopped = complete no-op (2026-07-27, see
    // ImsPlayer::stop - every press used to ship another audible CmdReset).
    const bool bWasActive = m_playing.load() || m_currentTick.load() != 0
            || m_position.load() != 0;
    if (!bWasActive) {
        m_playing.store(false);
        return;
    }

    std::lock_guard<std::mutex> lock(m_playerMutex);
    m_playing.store(false);
    m_position.store(0);
    m_positionRemainder = 0.0;
    m_currentTick.store(0);
    m_sampleCounter.store(0.0f);
    m_lpfLastL = 0.0f; m_lpfLastR = 0.0f;
    m_needsFadeIn.store(false);
    m_fadeCounter.store(0);
    // Key-off first so the DEVICE at the far end of the tunnel gets a normal
    // note-off and releases naturally, then reset the local chip (instant
    // silence, no ringing tail) and rewind with the tunnel suppressed, so the
    // reset and the driver's re-init do not have to be streamed over a 31250
    // baud link. Unsuppressing arms the resync that rebuilds device state on
    // the next play.
    if (m_opl) m_opl->silenceAllVoices();
    OplTunnelSender::instance().setSuppressed(true);
    if (m_opl) m_opl->init();
    if (m_backend) m_backend->rewind(0);
    OplTunnelSender::instance().setSuppressed(false);
    for (int i = 0; i < 18; ++i) m_voiceLevelOut[i].store(0);
    for (int i = 0; i < 64; ++i) m_instLevelOut[i].store(0);
    memset(m_cachedVoiceInsts, 0, sizeof(m_cachedVoiceInsts));
    memset(m_cachedVoiceNotes, 0, sizeof(m_cachedVoiceNotes));
    memset(m_cachedVoiceVols, 0, sizeof(m_cachedVoiceVols));
    memset(m_cachedVoiceKeyOn, 0, sizeof(m_cachedVoiceKeyOn));
}

void OkaPlayer::setPosition(unsigned long positionMs)
{
    if (!m_backend || positionMs >= m_duration) return;
    std::lock_guard<std::mutex> lock(m_playerMutex);

    // OPL tunnel: suppress the wire during the fast-forward (see
    // ImsPlayer::setPosition) - unsuppress below resyncs via snapshot.
    OplTunnelSender::instance().setSuppressed(true);

    m_backend->rewind(0);
    double accMs = 0.0;
    unsigned long ticks = 0;
    while (accMs < (double)positionMs && ticks < 1000000) {
        float r = m_backend->getrefresh();
        if (r < 1.0f) r = 120.0f;
        accMs += 1000.0 / (double)r;
        if (!m_backend->update()) break;
        ++ticks;
    }
    float r = m_backend->getrefresh();
    if (r < 1.0f) r = 120.0f;
    m_currentTick.store(ticks);
    m_sampleCounter.store((float)((double)m_sampleRate / (double)r));
    m_position.store(positionMs);
    m_positionRemainder = 0.0;
    m_needsFadeIn.store(true);
    m_fadeCounter.store(0);
    // Clear the DSP low-pass filter state so its stale value doesn't leak
    // through as a click/thump when playback resumes after the seek.
    m_lpfLastL = 0.0f; m_lpfLastR = 0.0f;

    OplTunnelSender::instance().setSuppressed(false);
    emit positionChanged(positionMs);
}

unsigned long OkaPlayer::getPosition() const { return m_position.load(); }
void OkaPlayer::setVolume(int v)    { m_volume.store(std::clamp(v, 0, 127)); }
void OkaPlayer::setDspLevel(int l)  { m_dspLevel.store(std::clamp(l, 0, 3)); }

QList<int> OkaPlayer::getVoiceVolumes() const
{
    QList<int> v;
    for (int i = 0; i < 20; ++i) {
        v.append(m_voiceLevelOut[i].load(std::memory_order_relaxed));
    }
    return v;
}

QList<int> OkaPlayer::getInstrumentVolumes() const
{
    QList<int> v;
    int n = m_instrumentNames.size();
    if (n > 63) n = 63;
    for (int i = 0; i < n; ++i) {
        if (m_instrumentNames[i].isEmpty()) {
            v.append(0);
        } else {
            v.append(m_instLevelOut[i].load(std::memory_order_relaxed));
        }
    }
    return v;
}

QStringList OkaPlayer::getVoiceInstrumentNames() const
{
    QStringList names;
    for (int i = 0; i < 20; ++i) {
        char instNameBuf[32];
        char noteStrBuf[8];
        memcpy(instNameBuf, m_cachedVoiceInsts[i], 32);
        memcpy(noteStrBuf, m_cachedVoiceNotes[i], 8);
        instNameBuf[31] = '\0';
        noteStrBuf[7] = '\0';
        
        QString instName = QString::fromLocal8Bit(instNameBuf);
        QString noteStr = QString::fromLocal8Bit(noteStrBuf);
        int vol = m_cachedVoiceVols[i];
        bool isOn = m_cachedVoiceKeyOn[i] != 0;
        
        if (instName.isEmpty()) {
            names.append(".|   |  0|0");
        } else {
            QString volStr = QString("%1").arg(vol, 3);
            QString isOnStr = isOn ? "1" : "0";
            names.append(QString("%1|%2|%3|%4").arg(instName).arg(noteStr).arg(volStr).arg(isOnStr));
        }
    }
    return names;
}



void OkaPlayer::renderAudio(float* output, unsigned int frameCount)
{
    // Non-blocking lock: while a seek re-simulates the song on the UI thread it
    // holds this mutex for many ms. Blocking the audio thread on it would
    // underrun the device → a "click". Instead ramp the last emitted sample
    // down to silence (no step = no click), then let the post-seek fade-in
    // resume cleanly.
    std::unique_lock<std::mutex> lock(m_playerMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        for (unsigned int i = 0; i < frameCount; ++i) {
            float t = 1.0f - (float)i / (float)frameCount;
            output[i * 2]     = m_lastOutL * t;
            output[i * 2 + 1] = m_lastOutR * t;
        }
        m_lastOutL = 0.0f; m_lastOutR = 0.0f;
        return;
    }
    if (!m_backend || !m_opl || !m_playing.load()) {
        // Anti-click on STOP (2026-07-27, ImsPlayer parity): ramp the last
        // emitted sample to 0 across this first stopped buffer instead of an
        // instant jump to silence.
        for (unsigned int i = 0; i < frameCount; ++i) {
            float t = 1.0f - (float)i / (float)frameCount;
            output[i * 2]     = m_lastOutL * t;
            output[i * 2 + 1] = m_lastOutR * t;
        }
        m_lastOutL = 0.0f; m_lastOutR = 0.0f;
        return;
    }

    short tmp[16384];
    unsigned int framesToRender = std::min(frameCount, 8192u);

    float current = m_sampleCounter.load() + (float)framesToRender;
    int maxTicks = 200;
    bool ticked = false;

    while (maxTicks-- > 0) {
        float refresh = m_backend->getrefresh();
        if (refresh < 1.0f) refresh = 120.0f;
        float samplesPerTick = (float)m_sampleRate / refresh;
        if (current < samplesPerTick) break;
        // OPL tunnel: stamp this tick's register writes with the song clock so
        // the jukebox replays them with the exact tick timing (one timed batch
        // per tick - see OplTunnelSender::setNowMs). Same as ImsPlayer.
        OplTunnelSender::instance().setNowMs(
            (uint32_t)(m_tunnelClockSamples * 1000.0 / (double)m_sampleRate));
        if (!m_backend->update()) {
            m_playing.store(false);
            QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
            break;
        }
        m_tunnelClockSamples += (double)samplesPerTick;
        current -= samplesPerTick;
        m_currentTick.fetch_add(1);
        ticked = true;
    }

    m_opl->update(tmp, framesToRender);

    const float volScale = m_volume.load() / 100.0f;
    const int dspLevel = m_dspLevel.load();
    float alpha = 1.0f, drive = 1.0f;
    if (dspLevel == 1) { alpha = 0.80f; drive = 1.05f; }
    else if (dspLevel == 2) { alpha = 0.55f; drive = 1.30f; }
    else if (dspLevel == 3) { alpha = 0.35f; drive = 1.65f; }

    for (unsigned int i = 0; i < framesToRender; ++i) {
        float l = (float)tmp[i * 2]     / 32768.0f;
        float r = (float)tmp[i * 2 + 1] / 32768.0f;
        if (m_needsFadeIn.load()) {
            int fc = m_fadeCounter.load();
            // Post-seek resume: stay fully muted for the first ~30ms while the
            // OPL renders past its transient (phase/envelope discontinuity from
            // the seek's register jump), then ramp up over ~60ms. This masks
            // the "click" that a short fade alone left audible.
            const int kMute = 1500;
            const int kFade = 3000;
            float fade = (fc < kMute) ? 0.0f : (float)(fc - kMute) / (float)kFade;
            if (fade >= 1.0f) { fade = 1.0f; m_needsFadeIn.store(false); }
            l *= fade; r *= fade;
            m_fadeCounter.store(fc + 1);
        }
        if (dspLevel > 0) {
            l = alpha * l + (1.0f - alpha) * m_lpfLastL;
            r = alpha * r + (1.0f - alpha) * m_lpfLastR;
            m_lpfLastL = l; m_lpfLastR = r;
            auto softClip = [](float x) {
                if (x > 1.0f) return 1.0f;
                if (x < -1.0f) return -1.0f;
                return x - (x * x * x) / 3.0f;
            };
            l = softClip(l * drive);
            r = softClip(r * drive);
        }
        if (OplTunnelSender::instance().isEnabled()) {
            output[i * 2]     = 0.0f;
            output[i * 2 + 1] = 0.0f;
        } else {
            output[i * 2]     = l * volScale;
            output[i * 2 + 1] = r * volScale;
        }
    }
    if (framesToRender < frameCount) {
        std::memset(output + framesToRender * 2, 0, (frameCount - framesToRender) * 2 * sizeof(float));
    }
    if (framesToRender > 0) {
        m_lastOutL = output[(framesToRender - 1) * 2];
        m_lastOutR = output[(framesToRender - 1) * 2 + 1];
    }

    m_sampleCounter.store(current);
    // Song position. Accumulate the fraction rather than truncating it: one
    // buffer at the device's default period is a fraction under a whole
    // millisecond, and dropping that fraction every callback cost about a tenth
    // of the clock - SOVIRGIN.GYB, 2:43 by the load-time scan, reported 2:26
    // when the audio stopped, so the progress bar froze short of the end while a
    // seek (which stores the position directly) made it agree again. ImsPlayer
    // has carried this remainder for a while; GYB and OKA never got it. The
    // tempo factor matches Ims too: m_duration is measured once at load, so at
    // 150% the position has to advance 1.5x real time to reach that same end.
    m_positionRemainder += ((double)framesToRender * 1000.0 / (double)m_sampleRate)
                         * ((double)(m_backend ? m_backend->userTempoScale() : 100) / 100.0);
    if (m_positionRemainder >= 1.0) {
        unsigned long msToAdd = (unsigned long)m_positionRemainder;
        m_position.fetch_add(msToAdd);
        m_positionRemainder -= (double)msToAdd;
    }

    if (ticked) {
        for (int i = 0; i < 16; ++i) {
            int vol = m_backend ? m_backend->getvoicevolume(i) : 0;
            m_voiceLevelOut[i].store(vol, std::memory_order_relaxed);
        }
        for (int i = 16; i < 20; ++i) {
            m_voiceLevelOut[i].store(0, std::memory_order_relaxed);
        }

        int perInst[64]; for (int i = 0; i < 64; ++i) perInst[i] = 0;
        if (m_backend) {
            for (int i = 0; i < 16; ++i) { // OKA는 16개 채널
                int chVol = m_voiceLevelOut[i].load(std::memory_order_relaxed);
                if (chVol > 0) {
                    int slot = m_backend->voiceSlot(i);
                    if (slot >= 0 && slot < 64) {
                        if (chVol > perInst[slot]) perInst[slot] = chVol;
                    }
                }
            }
        }
        for (int i = 0; i < 64; ++i) m_instLevelOut[i].store(perInst[i], std::memory_order_relaxed);

        // Visualizer Lock-Free Cache Update - at display speed only (~30/s),
        // not once per tick; see m_uiSnapshotFrames in the header.
        m_uiSnapshotFrames += framesToRender;
        bool bRefreshUi = false;
        if (m_uiSnapshotFrames >= (unsigned)(m_sampleRate / 30)) {
            m_uiSnapshotFrames = 0;
            bRefreshUi = true;
        }
        if (bRefreshUi && m_backend && m_opl) {
            InterceptingOpl* opl = static_cast<InterceptingOpl*>(m_opl);
            for (int i = 0; i < 18; ++i) {
                QString instName = "";
                if (i < m_backend->getnumvoices()) {
                    instName = QString::fromLocal8Bit(m_backend->getvoiceinstrument(i).c_str()).trimmed();
                }
                bool nameOk = !instName.isEmpty();
                if (nameOk) {
                    for (const QChar& ch : instName) {
                        ushort u = ch.unicode();
                        bool ok = (u >= 0x20 && u <= 0x7E)
                               || (u >= 0xAC00 && u <= 0xD7A3)
                               || (u >= 0x3130 && u <= 0x318F);
                        if (!ok) { nameOk = false; break; }
                    }
                }
                if (!nameOk) instName = "";
                if (instName.isEmpty()) instName = "--";

                if (opl->m_drumMode && i >= 6 && i <= 10) {
                    static const char* defaultDrums[] = { "BD", "SD", "TM", "CY", "HH" };
                    if (instName == "--" || instName == "Unknown" || instName.contains("Voice")) {
                        instName = defaultDrums[i - 6];
                    }
                }

                QByteArray utf8Name = instName.toLocal8Bit();
                strncpy(m_cachedVoiceInsts[i], utf8Name.constData(), 31);
                m_cachedVoiceInsts[i][31] = '\0';

                int vol = m_oplVolume[i];
                bool isOn = m_oplKeyOn[i];
                m_cachedVoiceVols[i] = (uint8_t)vol;
                m_cachedVoiceKeyOn[i] = isOn ? 1 : 0;

                QString noteStr = "   ";
                int fnum = m_oplRegA[i] | ((m_oplRegB[i] & 0x03) << 8);
                int block = (m_oplRegB[i] >> 2) & 0x07;
                if (isOn && fnum > 0) {
                    double freq = (double)fnum * std::pow(2.0, block - 20) * 49716.0;
                    double noteVal = 12.0 * std::log2(freq / 440.0) + 69.0;
                    int note = qBound(0, (int)std::round(noteVal), 127);
                    
                    static const char* noteNames[] = {
                        "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
                    };
                    int octave = (note / 12) - 1;
                    noteStr = QString("%1%2").arg(noteNames[note % 12]).arg(octave);
                }
                QByteArray utf8Note = noteStr.toLocal8Bit();
                strncpy(m_cachedVoiceNotes[i], utf8Note.constData(), 7);
                m_cachedVoiceNotes[i][7] = '\0';
            }
            for (int i = 18; i < 20; ++i) {
                m_cachedVoiceInsts[i][0] = '\0';
                m_cachedVoiceNotes[i][0] = '\0';
                m_cachedVoiceVols[i] = 0;
                m_cachedVoiceKeyOn[i] = 0;
            }
        }
    }
}



void OkaPlayer::setUserTempoScale(int scale)
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_backend) {
        m_backend->setUserTempoScale(scale);
    }
}

int OkaPlayer::getUserTempoScale() const
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    return m_backend ? m_backend->userTempoScale() : 100;
}

void OkaPlayer::setUserKeyTranspose(int semitones)
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_backend) {
        m_backend->setUserKeyTranspose(semitones);
    }
}

int OkaPlayer::getUserKeyTranspose() const
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    return m_backend ? m_backend->userKeyTranspose() : 0;
}

int OkaPlayer::getCurrentBpm() const
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    return m_backend ? m_backend->getCurrentBpm() : 120;
}

void OkaPlayer::forceUpdateOplStereo()
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_opl) {
        m_opl->CNemuopl::write(0x105, 0x01); // Ensure OPL3 mode is ON
        for (int ch = 0; ch < 18; ++ch) {
            int reg = ((ch >= 9) ? 0x100 : 0) + 0xC0 + (ch % 9);
            int origVal = m_opl->originalPanReg[ch];
            // Mode 10 places the two chips itself - the map's letters are the
            // mirror of the chip's bits, so going through getChannelPanBit()
            // here put chip 0 on the LEFT while the write path had put it on
            // the right, and switching mode mid-song stacked both chips on one
            // side (the left went muffled until the song was restarted).
            const bool dualChip = JJoMeSynth::instance().getOplStereoMode() == 10;
            int panBit = dualChip ? 0x20
                                  : JJoMeSynth::instance().getChannelPanBit(ch);
            int finalVal = (origVal & ~0x30) | panBit;
            m_opl->CNemuopl::write(reg, finalVal);
        }
        if (JJoMeSynth::instance().getOplStereoMode() == 10)
            m_opl->rebuildSecondChip();
        else
            m_opl->silenceSecondChip();
    }
}
