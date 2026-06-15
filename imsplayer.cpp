#include "imsplayer.h"
#include <QDebug>
#include <QFileInfo>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <chrono>
#include <cstdio>
#include <windows.h>
#include "jjomesynth.h"
#include "okafilehandler.h"


// Include mus.h for direct CmusPlayer usage
#include <adplug/composer.h>
#include <adplug/mus.h>
#include <adplug/sop.h>   // CsopPlayer: exact per-voice instrument (patched)
#include <adplug/rol.h>   // CrolPlayer: exact per-voice instrument (patched)

// Decode an adplug instrument name (Johab / CP1361) to a QString, matching the
// encoding used when m_instruments is built in loadFile().
static QString decodeInstNameCp1361(const std::string& raw)
{
    if (raw.empty()) return QString();
    int wlen = MultiByteToWideChar(1361, 0, raw.c_str(), (int)raw.length(), NULL, 0);
    if (wlen > 0) {
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(1361, 0, raw.c_str(), (int)raw.length(), &w[0], wlen);
        return QString::fromStdWString(w);
    }
    return QString::fromLocal8Bit(raw.c_str());
}

class InterceptingOpl : public CNemuopl {
public:
    InterceptingOpl(int rate, bool bit16, bool usestereo, std::atomic<int>* userKeyTranspose)
        : CNemuopl(rate), m_userKeyTranspose(userKeyTranspose) {
        
        // Initialize operator mapping table
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
        CNemuopl::init();
        // Deep Reset: Zero out all OPL3 registers (Bank 0 & 1) to prevent state leakage
        for (int i = 0x00; i <= 0xFF; ++i) {
            CNemuopl::write(i, 0);
            CNemuopl::write(i | 0x100, 0);
        }
        // Explicitly enable OPL3 mode (Bit 5 of 0x01 in Bank 1)
        CNemuopl::write(0x101, 0x20);
        CNemuopl::write(0x105, 0x01); // OPL3 Mode Enable (Bit 0 = 1, required for stereo)
        clearInternal();
    }

    void clearInternal() {
        memset(keyOn, 0, sizeof(keyOn));
        memset(volume, 0, sizeof(volume));
        memset(attack, 0, sizeof(attack));
        memset(regA, 0, sizeof(regA));
        memset(regB, 0, sizeof(regB));
        m_drumMode = false;
        for (int i = 0; i < 18; ++i) {
            originalPanReg[i] = 0x30; // Default to Center (Both)
            m_shadowVoices[i] = ShadowVoice();
        }
    }

    void applyTranspose(int ch, int& rA, int& rB) {
        int transpose = m_userKeyTranspose->load(std::memory_order_relaxed);
        if (transpose == 0) return;

        // 드럼 모드이고 타악기 채널(보이스 6~10)인 경우 조옮김 패스
        if (m_drumMode && (ch >= 6 && ch <= 10)) {
            return;
        }

        int fnum = rA | ((rB & 0x03) << 8);
        int block = (rB >> 2) & 0x07;

        if (fnum == 0) return;

        double ratio = pow(2.0, (double)transpose / 12.0);
        double temp = (double)fnum * pow(2.0, (double)block) * ratio;

        int newBlock = 0;
        double newFnum = temp;

        while (newFnum >= 1024.0 && newBlock < 7) {
            newFnum /= 2.0;
            newBlock++;
        }

        int finalFnum = qBound(0, (int)round(newFnum), 1023);

        rA = finalFnum & 0xFF;
        rB = (rB & 0xE0) | ((finalFnum >> 8) & 0x03) | (newBlock << 2);
    }

    void write(int reg, int val) override {
        int bankOffset = (reg >= 0x100 || getchip() == 1) ? 9 : 0;
        int baseReg = reg & 0xFF;

        // OPL3 모드 끄기(OPL2 복원) 방지 필터링
        if (reg == 0x105) {
            val |= 0x01;
        }

        // 0. OPL3 Panning (0xC0~0xC8) 가로채기 및 유사 스테레오 변조
        if (baseReg >= 0xC0 && baseReg <= 0xC8) {
            int ch = bankOffset + (baseReg - 0xC0);
            if (ch < 18) {
                originalPanReg[ch] = val; // Store original register value
                int panBit = JJoMeSynth::instance().getChannelPanBit(ch);
                val &= ~0x30;
                val |= panBit;
            }
        }

        // 섀도잉 레지스터 업데이트 (악기 추적용)
        int bank = (reg >= 0x100 || getchip() == 1) ? 0x100 : 0;
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

        // 2. 주파수 레지스터 가로채기 및 조옮김 보정
        if (baseReg >= 0xA0 && baseReg <= 0xA8) {
            int ch = bankOffset + (baseReg - 0xA0);
            if (ch < 18) {
                regA[ch] = val;
                int finalVal = val;
                int finalRegB = regB[ch];
                applyTranspose(ch, finalVal, finalRegB);
                
                CNemuopl::write(reg, finalVal);
                CNemuopl::write(0xB0 + (baseReg - 0xA0) + bank, finalRegB);
                return;
            }
        }
        else if (baseReg >= 0xB0 && baseReg <= 0xB8) {
            int ch = bankOffset + (baseReg - 0xB0);
            if (ch < 18) {
                regB[ch] = val;
                
                int finalRegA = regA[ch];
                int finalVal = val;
                applyTranspose(ch, finalRegA, finalVal);

                CNemuopl::write(0xA0 + (baseReg - 0xB0) + bank, finalRegA);
                CNemuopl::write(reg, finalVal);
                
                val = finalVal; // 아래 레벨 미터 감지 로직의 KeyOn 비트 분석을 위해 동기화
            }
        }

        CNemuopl::write(reg, val);
        
        // Channel Key-On (0xB0 - 0xB8)
        if (baseReg >= 0xB0 && baseReg <= 0xB8) {
            int ch = bankOffset + (baseReg - 0xB0);
            bool wasOn = keyOn[ch];
            keyOn[ch] = (val & 0x20) != 0;
            // Onset (keyon rising edge) → record a meter attack peak. Used by
            // the onset+decay level meter (IMS/ROL); SOP keeps the old meter.
            if (onsetMode && !wasOn && keyOn[ch]) {
                int pk = volume[ch] > 0 ? volume[ch] : 80;
                if (pk > attack[ch]) attack[ch] = pk;
            }
            // Removed: if (!keyOn[ch]) volume[ch] = 0; to preserve OPL register state
        }
        // Drum Mode Key-On (0xBD) - Only for Bank 1 (OPL2 compatible)
        else if (reg == 0xBD) {
            bool drumMode = (val & 0x20) != 0;
            if (drumMode) {
                static const int drumCh[5] = {6, 7, 8, 9, 10};
                static const int drumBit[5] = {0x10, 0x08, 0x04, 0x02, 0x01};
                for (int k = 0; k < 5; ++k) {
                    int ch = drumCh[k];
                    bool wasOn = keyOn[ch];
                    keyOn[ch] = (val & drumBit[k]) != 0;
                    if (onsetMode && !wasOn && keyOn[ch]) {
                        int pk = volume[ch] > 0 ? volume[ch] : 80;
                        if (pk > attack[ch]) attack[ch] = pk;
                    }
                }
            }
        }
        // Volume (Total Level) - Check both modulator and carrier
        else if (baseReg >= 0x40 && baseReg <= 0x55) {
            static const int opToCh[22] = {
                0, 1, 2, 0, 1, 2, -1, -1, 3, 4, 5, 3, 4, 5, -1, -1, 6, 7, 8, 6, 7, 8
            };
            int op = baseReg - 0x40;
            if (op < 22) {
                int ch = opToCh[op];
                if (ch != -1) {
                    int realCh = bankOffset + ch;
                    int tl = val & 0x3F;
                    int v = ((63 - tl) * 127) / 63;
                    if (v > volume[realCh]) volume[realCh] = v;
                }
            }
            // Additional mapping for percussion operators in drum mode
            if (baseReg == 0x51) volume[7] = ((63 - (val & 0x3F)) * 127) / 63; // SD TL
            if (baseReg == 0x52) volume[10] = ((63 - (val & 0x3F)) * 127) / 63; // HH TL
            if (baseReg == 0x55) volume[9] = ((63 - (val & 0x3F)) * 127) / 63; // CYM TL
        }
    }

    bool keyOn[18];
    int  volume[18];
    int  attack[18];      // per-voice note-onset peak (onset+decay meter)
    bool onsetMode = false; // true for IMS/ROL (onset+decay meter); false for SOP
    
    // 조옮김(Key Change) 지원 섀도 필드
    unsigned char regA[18];
    unsigned char regB[18];
    bool m_drumMode;
    std::atomic<int>* m_userKeyTranspose;
    int originalPanReg[18];

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
};

ImsPlayer::ImsPlayer(QObject *parent)
    : QObject(parent)
    , m_player(nullptr)
    , m_opl(nullptr)
    , m_playing(false)
    , m_duration(0)
    , m_sampleRate(49716)
    , m_position(0)
    , m_sampleCounter(0.0f)
    , m_volume(100)
    , m_basicTempo(120)
    , m_nTickBeat(240)
    , m_currentTick(0)
    , m_dspLevel(0)
    , m_lpfLastL(0.0f)
    , m_lpfLastR(0.0f)
    , m_needsFadeIn(false)
    , m_fadeCounter(0)
    , m_customRefresh(70.0f)
    , m_positionRemainder(0.0)
    , m_userTempoScale(100)
    , m_userKeyTranspose(0)
    , m_isRol(false)
{
    memset(m_cachedVoiceInsts, 0, sizeof(m_cachedVoiceInsts));
    memset(m_cachedVoiceNotes, 0, sizeof(m_cachedVoiceNotes));
    memset(m_cachedVoiceVols, 0, sizeof(m_cachedVoiceVols));
    memset(m_cachedVoiceKeyOn, 0, sizeof(m_cachedVoiceKeyOn));
}

ImsPlayer::~ImsPlayer()
{
    stop();
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_player) delete m_player;
    if (m_opl) delete m_opl;
}

// IMS 파일 헤더 오프셋:
// 0: majorVersion (1 byte)
// 1: minorVersion (1 byte)
// 2: tuneId      (4 bytes)
// 6: tuneName    (30 bytes, TUNE_NAME_SIZE)
static const int IMS_TITLE_OFFSET = 6;
static const int IMS_TITLE_SIZE   = 30;

QString ImsPlayer::extractTitleQuick(const QString& fileName)
{
    QString lower = fileName.toLower();

    if (lower.endsWith(".ims")) {
        // IMS: 헤더만 읽어 tuneName 추출 (완전 로딩 불필요)
        QFile f(fileName);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        if (f.size() < IMS_TITLE_OFFSET + IMS_TITLE_SIZE) return QString();

        f.seek(IMS_TITLE_OFFSET);
        QByteArray raw = f.read(IMS_TITLE_SIZE);
        f.close();

        // 조합형 한글(EUC-KR / KS-1361) → UTF-16 변환 시도
        QString title;
        int wideLen = MultiByteToWideChar(1361, 0, raw.constData(), raw.size(), NULL, 0);
        if (wideLen > 0) {
            std::wstring wideStr(wideLen, L'\0');
            MultiByteToWideChar(1361, 0, raw.constData(), raw.size(), &wideStr[0], wideLen);
            title = QString::fromStdWString(wideStr);
        } else {
            title = QString::fromLocal8Bit(raw.constData(), strnlen(raw.constData(), IMS_TITLE_SIZE));
        }
        title = title.trimmed();

        // 파일명과 동일하면 의미 없으므로 반환 안 함
        if (title.isEmpty() || title == QFileInfo(fileName).baseName()) return QString();
        return title;
    }

    if (lower.endsWith(".sop")) {
        // SOP는 AdPlug 팩토리에서 로딩 후 타이틀을 가져오므로 여기서는 빈 문자열 반환 (나중에 전체 로드 시 표시됨)
        return QString();
    }

    // ROL 파일은 헤더에 별도 제목이 없으므로 빈 문자열 반환
    return QString();
}

QString ImsPlayer::extractTitleQuick(const QByteArray& fileData, const QString& ext)
{
    QString lower = ext.toLower();

    if (lower.endsWith(".ims")) {
        if (fileData.size() < IMS_TITLE_OFFSET + IMS_TITLE_SIZE) return QString();
        QByteArray raw = fileData.mid(IMS_TITLE_OFFSET, IMS_TITLE_SIZE);

        // 조합형 한글(EUC-KR / KS-1361) → UTF-16 변환 시도
        QString title;
        int wideLen = MultiByteToWideChar(1361, 0, raw.constData(), raw.size(), NULL, 0);
        if (wideLen > 0) {
            std::wstring wideStr(wideLen, L'\0');
            MultiByteToWideChar(1361, 0, raw.constData(), raw.size(), &wideStr[0], wideLen);
            title = QString::fromStdWString(wideStr);
        } else {
            title = QString::fromLocal8Bit(raw.constData(), strnlen(raw.constData(), IMS_TITLE_SIZE));
        }
        title = title.trimmed();
        return title;
    }
    return QString();
}

class RollExtractingOpl : public CNemuopl {
public:
    RollExtractingOpl(int rate) : CNemuopl(rate), currentTick(0), notesList(nullptr) {
        memset(fnum, 0, sizeof(fnum));
        memset(block, 0, sizeof(block));
    }
    uint64_t currentTick;
    QList<RollNote>* notesList;
    int fnum[18];
    int block[18];

    void write(int reg, int val) override {
        CNemuopl::write(reg, val);
        int bankOffset = (reg >= 0x100) ? 9 : 0;
        int baseReg = reg & 0xFF;

        if (baseReg >= 0xA0 && baseReg <= 0xA8) {
            int ch = bankOffset + (baseReg - 0xA0);
            fnum[ch] = (fnum[ch] & 0x300) | val;
        } else if (baseReg >= 0xB0 && baseReg <= 0xB8) {
            int ch = bankOffset + (baseReg - 0xB0);
            fnum[ch] = (fnum[ch] & 0xFF) | ((val & 0x03) << 8);
            block[ch] = (val >> 2) & 0x07;
            bool keyOn = (val & 0x20) != 0;
            
            if (keyOn && notesList) {
                // calculate pitch
                double freq = ((double)fnum[ch] * 49716.0) / (double)(1 << (20 - block[ch]));
                int pitch = 60; // default
                if (freq > 0.1) {
                    pitch = (int)round(69.0 + 12.0 * log2(freq / 440.0));
                }
                notesList->append({currentTick, ch, pitch});
            }
        } else if (reg == 0xBD) {
            bool drumMode = (val & 0x20) != 0;
            if (drumMode && notesList) {
                if (val & 0x10) notesList->append({currentTick, 6, 36}); // BD -> Ch 7 (index 6)
                if (val & 0x08) notesList->append({currentTick, 7, 38}); // SD
                if (val & 0x04) notesList->append({currentTick, 8, 43}); // TOM
                if (val & 0x02) notesList->append({currentTick, 9, 49}); // CYMB
                if (val & 0x01) notesList->append({currentTick, 10, 42}); // HH
            }
        }
    }
};

bool ImsPlayer::loadFile(const QString &fileName)
{
    stop();
    
    std::lock_guard<std::mutex> lock(m_playerMutex);
    
    if (!QFile::exists(fileName)) return false;

    if (m_player) { delete m_player; m_player = nullptr; }
    if (m_opl) { delete m_opl; m_opl = nullptr; }

    m_opl = new InterceptingOpl(m_sampleRate, true, true, &m_userKeyTranspose);
    // Enable the onset+decay level meter for all OPL formats including SOP.
    static_cast<InterceptingOpl*>(m_opl)->onsetMode = true;

    m_provider.reset();
    m_provider.setMainFile(fileName);
    if (!m_externalBankPath.isEmpty()) {
        m_provider.setExternalBank(m_externalBankPath);
    }
    CmusPlayer* musPlayer = new CmusPlayer(m_opl);
    if (musPlayer->load(QFile::encodeName(fileName).constData(), m_provider)) {
        m_player = musPlayer;
        m_bankName = m_provider.loadedBankFile;
    } else {
        delete musPlayer;
        m_player = CAdPlug::factory(QFile::encodeName(fileName).constData(), m_opl, CAdPlug::players, m_provider);
        m_bankName = m_provider.loadedBankFile;
    }
    
    if (!m_player) return false;

    // [ROLL EXTRACT] Use a DUMMY OPL to extract piano roll notes and calculate duration.
    // This also prevents m_opl contamination for SOP files.
    m_rollNotes.clear();
    
    // Step 1: rewind() on real player
    m_player->rewind();
    
    // Step 2: getrefresh()
    m_customRefresh = m_player->getrefresh();
    if (m_customRefresh < 1.0f) m_customRefresh = 70.0f;
    
    // Step 3: Pre-scan using a SEPARATE dummy OPL
    {
        RollExtractingOpl* dummyOpl = new RollExtractingOpl(m_sampleRate);
        dummyOpl->notesList = &m_rollNotes;
        
        ImsFileProvider dummyProvider;
        dummyProvider.setMainFile(fileName);
        if (!m_externalBankPath.isEmpty()) dummyProvider.setExternalBank(m_externalBankPath);
        
        CPlayer* dummyPlayer = CAdPlug::factory(
            QFile::encodeName(fileName).constData(), dummyOpl, CAdPlug::players, dummyProvider);
        
        unsigned long totalTicks = 0;
        if (dummyPlayer) {
            dummyPlayer->rewind();
            // Arbitrary limit 1 million ticks (~4 hours at 70Hz) to prevent infinite loops
            while (dummyPlayer->update() && totalTicks < 1000000) {
                dummyOpl->currentTick = totalTicks;
                totalTicks++;
            }
            delete dummyPlayer;
        }
        delete dummyOpl;
        
        if (fileName.toLower().endsWith(".sop") || fileName.toLower().endsWith(".ims")) {
            m_duration = (unsigned long)((double)totalTicks * 1000.0 / (double)m_customRefresh);
        } else {
            m_duration = m_player->songlength();
        }
    }
    
    // Step 4: rewind real player once cleanly
    m_player->rewind();

    if (m_duration < 1000 || m_duration > 3600000) m_duration = 300000;
    
    std::string rawTitle = m_player->gettitle();
    int wideLen = MultiByteToWideChar(1361, 0, rawTitle.c_str(), rawTitle.length(), NULL, 0);
    if (wideLen > 0) {
        std::wstring wideStr(wideLen, L'\0');
        MultiByteToWideChar(1361, 0, rawTitle.c_str(), rawTitle.length(), &wideStr[0], wideLen);
        m_title = QString::fromStdWString(wideStr);
    } else {
        m_title = QString::fromLocal8Bit(rawTitle.c_str());
    }
    
    if (m_title.isEmpty()) m_title = QFileInfo(fileName).baseName();
    
    m_instruments.clear();
    unsigned int numInsts = m_player->getinstruments();
    for (unsigned int i = 0; i < numInsts; ++i) {
        std::string rawInst = m_player->getinstrument(i);
        if (!rawInst.empty()) {
            int wideLenInst = MultiByteToWideChar(1361, 0, rawInst.c_str(), rawInst.length(), NULL, 0);
            if (wideLenInst > 0) {
                std::wstring wideStrInst(wideLenInst, L'\0');
                MultiByteToWideChar(1361, 0, rawInst.c_str(), rawInst.length(), &wideStrInst[0], wideLenInst);
                m_instruments.append(QString::fromStdWString(wideStrInst));
            } else {
                m_instruments.append(QString::fromLocal8Bit(rawInst.c_str()));
            }
        }
    }

    // IMS 파일 헤더를 직접 파싱하여 nBasicTempo, nTickBeat 추출 (IMS 전용)
    if (fileName.toLower().endsWith(".ims")) {
        QFile imsFile(fileName);
        if (imsFile.open(QIODevice::ReadOnly)) {
            QByteArray hdr = imsFile.read(70);
            imsFile.close();
            if (hdr.size() >= 70) {
                uint8_t  nTickBeat    = static_cast<uint8_t>(hdr[36]);  // offset 36
                uint16_t nBasicTempo  = 0;
                memcpy(&nBasicTempo, hdr.constData() + 60, 2);           // offset 60
                m_nTickBeat  = (nTickBeat  > 0) ? nTickBeat  : 240;
                if (nBasicTempo > 0) {
                    m_basicTempo = nBasicTempo;
                } else {
                    // 헤더 템포가 0일 경우, AdPlug 플레이어가 로딩 시점에 분석한 실제 재생 주파수(Hz)로부터 템포를 역산합니다.
                    // Hz = basicTempo * nTickBeat / 60  ->  basicTempo = Hz * 60 / nTickBeat
                    m_basicTempo = (int)((m_customRefresh * 60.0) / (double)m_nTickBeat + 0.5);
                    if (m_basicTempo <= 0) m_basicTempo = 120;
                    qDebug() << "[ImsPlayer] Header tempo is 0! Recalculated from getrefresh():" 
                             << m_customRefresh << "Hz -> basicTempo =" << m_basicTempo;
                }
            }
        }
    }

    m_position = 0;
    
    // Set m_sampleCounter so that the first logic tick is processed IMMEDIATELY
    // on the first audio callback, preventing the 'chik' sound without skipping tick 0.
    float refresh = m_customRefresh;
    if (refresh < 1.0f) refresh = 70.0f;
    m_sampleCounter = (float)m_sampleRate / refresh;
    
    m_needsFadeIn = true;
    m_fadeCounter = 0;

    m_isSop = fileName.toLower().endsWith(".sop");
    m_isRol = fileName.toLower().endsWith(".rol");
    m_isIms = fileName.toLower().endsWith(".ims");
    m_isVgm = fileName.toLower().endsWith(".vgm") || fileName.toLower().endsWith(".vgz");
    m_vgmPatchMap.clear();
    m_vgmNextPatchNum = 1;
    memset(m_cachedVoiceInsts, 0, sizeof(m_cachedVoiceInsts));
    memset(m_cachedVoiceNotes, 0, sizeof(m_cachedVoiceNotes));
    memset(m_cachedVoiceVols, 0, sizeof(m_cachedVoiceVols));
    memset(m_cachedVoiceKeyOn, 0, sizeof(m_cachedVoiceKeyOn));

    return true;
}

void ImsPlayer::play()
{
    m_needsFadeIn = true;
    m_fadeCounter = 0;
    m_playing = true;
}

void ImsPlayer::pause()
{
    m_playing = false;
}

void ImsPlayer::stop()
{
    m_playing = false;
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_player) m_player->rewind();
    // Removed m_opl->init() because rewind() already does SoundWarmInit() which resets registers.
    // Calling m_opl->init() here desyncs the hardware state (like 0x104 4OP mask) from the driver.
    
    // Clear UI monitoring state
    for (int i = 0; i < 18; ++i) m_voiceVolumes[i].store(0, std::memory_order_relaxed);
    for (int i = 0; i < 256; ++i) m_instVolumes[i].store(0, std::memory_order_relaxed);
    
    m_position = 0;
    m_sampleCounter = 0.0f;
    m_positionRemainder = 0.0;
    m_currentTick = 0;
    m_lpfLastL = 0.0f;
    m_lpfLastR = 0.0f;
}

void ImsPlayer::setPosition(unsigned long positionMs)
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_player) {
        m_player->rewind();
        // Removed m_opl->init() for the same reason as in stop()
        
        unsigned long targetTicks = 0;
        
        if (m_isVgm) {
            // VGM 포맷의 경우: 가변 틱 대응 정밀 시크 구현 (getrefresh() 누적 사용)
            double currentMs = 0.0;
            while (currentMs < (double)positionMs) {
                float refresh = m_player->getrefresh();
                if (refresh < 1.0f) refresh = 70.0f;
                currentMs += 1000.0 / (double)refresh;
                if (!m_player->update()) {
                    break;
                }
                targetTicks++;
            }
        } else {
            // 기존 고정 틱 포맷(IMS, ROL, SOP)의 경우
            float refresh = m_player->getrefresh();
            if (refresh < 1.0f) refresh = 70.0f;
            targetTicks = (unsigned long)((double)positionMs * refresh / 1000.0);
            for (unsigned long i = 0; i < targetTicks; ++i) {
                m_player->update();
            }
        }
        
        m_position = positionMs;
        float refresh = m_player->getrefresh();
        if (refresh < 1.0f) refresh = 70.0f;
        // Also trigger immediate update() upon resume to prevent audio pops
        m_sampleCounter = (float)m_sampleRate / refresh;
        m_positionRemainder = 0.0;
        m_currentTick = targetTicks;

        m_needsFadeIn = true;
        m_fadeCounter = 0;
        // Clear the DSP low-pass filter state so its stale value doesn't leak
        // through as a click/thump when playback resumes after the seek.
        m_lpfLastL = 0.0f; m_lpfLastR = 0.0f;
    }
}

void ImsPlayer::setVolume(int volume)
{
    m_volume.store(volume);
}

void ImsPlayer::setDspLevel(int level)
{
    m_dspLevel.store(level);
}

unsigned long ImsPlayer::getPosition() const
{
    return m_position.load();
}

QList<int> ImsPlayer::getVoiceVolumes() const
{
    QList<int> vols;
    for (int i = 0; i < 20; ++i) {
        vols.append(m_voiceVolumes[i].load(std::memory_order_relaxed));
    }
    return vols;
}

QList<int> ImsPlayer::getInstrumentVolumes() const
{
    QList<int> vols;
    vols.reserve(m_instruments.size());
    for (int i = 0; i < m_instruments.size(); ++i) {
        vols.append(m_instVolumes[i].load(std::memory_order_relaxed));
    }
    return vols;
}

void ImsPlayer::renderAudio(float* output, unsigned int frameCount)
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
    if (!m_playing || !m_player || !m_opl) {
        memset(output, 0, frameCount * 2 * sizeof(float));
        m_lastOutL = 0.0f; m_lastOutR = 0.0f;
        return;
    }

    short shortBuf[16384];
    unsigned int framesToRender = std::min(frameCount, 8192u);

    float currentCounter = m_sampleCounter.load();
    currentCounter += (float)framesToRender;

    int maxTicks = 100;
    bool logicUpdated = false;
    while (maxTicks-- > 0) {
        float refresh = m_player ? m_player->getrefresh() : 70.0f;
        if (refresh < 1.0f) refresh = 70.0f;
        float samplesPerTick = (float)m_sampleRate / (refresh * ((float)m_userTempoScale.load() / 100.0f));

        if (currentCounter < samplesPerTick) break;

        if (!m_player->update()) {
            m_playing = false;
            QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
            break;
        }
        currentCounter -= samplesPerTick;
        m_currentTick.fetch_add(1); 
        logicUpdated = true;
    }

    if (logicUpdated && m_player && m_opl) {
        InterceptingOpl* opl = static_cast<InterceptingOpl*>(m_opl);
        int numVoices = m_isSop ? 20 : (m_isRol ? 16 : 18);
        for (int i = 0; i < numVoices; ++i) {
            // 1. Get Instrument Name
            std::string rawName = "";
            if (m_isSop) {
                CsopPlayer* sop = static_cast<CsopPlayer*>(m_player);
                if (sop && i < (int)sop->getnumvoices()) {
                    rawName = sop->getvoiceinstrument(i);
                }
            } else if (m_isRol) {
                CrolPlayer* rol = static_cast<CrolPlayer*>(m_player);
                if (rol && i < (int)rol->getnumvoices()) {
                    rawName = rol->getvoiceinstrument(i);
                }
            } else if (m_isIms) {
                CcomposerBackend* comp = static_cast<CcomposerBackend*>(m_player);
                if (comp && i < (int)comp->getnumvoices()) {
                    rawName = comp->getvoiceinstrument(i);
                }
            } else {
                // VGM/VGZ 등의 OPL 패치 추적 가상 악기 이름 부여
                uint64_t patchKey = 0;
                const auto& sv = opl->m_shadowVoices[i];
                patchKey |= ((uint64_t)sv.ammulti_mod << 0);
                patchKey |= ((uint64_t)sv.ardr_mod     << 8);
                patchKey |= ((uint64_t)sv.slrr_mod     << 16);
                patchKey |= ((uint64_t)sv.wave_mod     << 24);
                patchKey |= ((uint64_t)sv.ammulti_car << 32);
                patchKey |= ((uint64_t)sv.ardr_car     << 40);
                patchKey |= ((uint64_t)sv.slrr_car     << 48);
                patchKey |= ((uint64_t)sv.wave_car     << 56);

                if (patchKey == 0) {
                    rawName = "--";
                } else {
                    int pNum = 0;
                    auto it = m_vgmPatchMap.find(patchKey);
                    if (it != m_vgmPatchMap.end()) {
                        pNum = it->second;
                    } else {
                        pNum = m_vgmNextPatchNum++;
                        m_vgmPatchMap[patchKey] = pNum;
                    }
                    rawName = "FM #" + std::to_string(pNum);
                }
            }
            QString instName = decodeInstNameCp1361(rawName).trimmed();
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

            // OPL Drum Mode (6-10) handling for non-SOP
            if (!m_isSop && opl->m_drumMode && i >= 6 && i <= 10) {
                static const char* defaultDrums[] = { "BD", "SD", "TM", "CY", "HH" };
                if (instName == "--" || instName == "Unknown" || instName.contains("Voice")) {
                    instName = defaultDrums[i - 6];
                }
            }
            
            QByteArray utf8Name = instName.toLocal8Bit();
            strncpy(m_cachedVoiceInsts[i], utf8Name.constData(), 31);
            m_cachedVoiceInsts[i][31] = '\0';

            // 2. Get Volume & KeyOn
            int vol = 0;
            bool isOn = false;
            if (m_isSop) {
                CsopPlayer* sop = static_cast<CsopPlayer*>(m_player);
                if (sop) {
                    vol = sop->getvoicevolume(i);
                    isOn = (vol > 0);
                }
            } else {
                isOn = opl->keyOn[i];
                vol = isOn ? opl->volume[i] : 0;
            }
            m_cachedVoiceVols[i] = (uint8_t)vol;
            m_cachedVoiceKeyOn[i] = isOn ? 1 : 0;

            // 3. Get Note Name
            QString noteStr = "   ";
            if (m_isSop) {
                CsopPlayer* sop = static_cast<CsopPlayer*>(m_player);
                if (sop && isOn) {
                    int note = sop->getvoicenote(i);
                    static const char* noteNames[] = {
                        "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
                    };
                    int octave = (note / 12) - 1;
                    noteStr = QString("%1%2").arg(noteNames[note % 12]).arg(octave);
                }
            } else {
                int oplCh = i;
                if (oplCh >= 0 && oplCh < 18) {
                    int fnum = opl->regA[oplCh] | ((opl->regB[oplCh] & 0x03) << 8);
                    int block = (opl->regB[oplCh] >> 2) & 0x07;
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
                }
            }
            QByteArray utf8Note = noteStr.toLocal8Bit();
            strncpy(m_cachedVoiceNotes[i], utf8Note.constData(), 7);
            m_cachedVoiceNotes[i][7] = '\0';
        }
    }

    // Render raw OPL audio
    m_opl->update(shortBuf, framesToRender);

    // [CRITICAL] Merged Processing Loop: Fade-in -> DSP -> Volume
    float volScale = m_volume.load() / 100.0f;
    int dspLevel = m_dspLevel.load();
    
    float alpha = 1.0f; 
    float drive = 1.0f;
    if (dspLevel == 1) { alpha = 0.80f; drive = 1.05f; }
    else if (dspLevel == 2) { alpha = 0.55f; drive = 1.30f; }
    else if (dspLevel == 3) { alpha = 0.35f; drive = 1.65f; }

    for (unsigned int i = 0; i < framesToRender; ++i) {
        float l = (float)shortBuf[i * 2] / 32768.0f;
        float r = (float)shortBuf[i * 2 + 1] / 32768.0f;
        
        // 1. Apply Fade-in
        if (m_needsFadeIn) {
            // Post-seek resume: stay fully muted for the first ~30ms while the
            // OPL renders past its transient, then ramp up over ~60ms. Masks
            // the seek "click" that a short fade alone left audible.
            const int kMute = 1500;
            const int kFade = 3000;
            float fade = (m_fadeCounter < kMute) ? 0.0f : (float)(m_fadeCounter - kMute) / (float)kFade;
            if (fade >= 1.0f) {
                fade = 1.0f;
                m_needsFadeIn = false;
            }
            l *= fade;
            r *= fade;
            m_fadeCounter++;
        }
        
        // 2. Apply Analog DSP (LPF + Saturation)
        if (dspLevel > 0) {
            // Low Pass Filter
            l = alpha * l + (1.0f - alpha) * m_lpfLastL;
            r = alpha * r + (1.0f - alpha) * m_lpfLastR;
            m_lpfLastL = l;
            m_lpfLastR = r;

            // Soft-clipping Saturation
            auto softClip = [](float x) {
                if (x > 1.0f) return 1.0f;
                if (x < -1.0f) return -1.0f;
                return x - (x * x * x) / 3.0f;
            };
            l = softClip(l * drive);
            r = softClip(r * drive);
        }
        
        // 3. Apply Main Volume and Output
        output[i * 2] = l * volScale;
        output[i * 2 + 1] = r * volScale;
    }
    
    // Fill remaining buffer with silence if needed
    if (framesToRender < frameCount) {
        memset(output + (framesToRender * 2), 0, (frameCount - framesToRender) * 2 * sizeof(float));
    }
    if (framesToRender > 0) {
        m_lastOutL = output[(framesToRender - 1) * 2];
        m_lastOutR = output[(framesToRender - 1) * 2 + 1];
    }

    m_sampleCounter.store(currentCounter);
    
    // Update visualizer state
    if (logicUpdated) {
        for (int i = 0; i < 256; ++i) {
            m_instVolumes[i].store(0, std::memory_order_relaxed);
        }
        
        if (m_isSop) {
            CsopPlayer* sopPlay = static_cast<CsopPlayer*>(m_player);
            for (int i = 0; i < 20; ++i) {
                m_voiceVolumes[i].store(sopPlay->getvoicevolume(i), std::memory_order_relaxed);
            }
        } else if (m_isRol || m_isIms) {
            CcomposerBackend* compPlay = static_cast<CcomposerBackend*>(m_player);
            for (int i = 0; i < 16; ++i) {
                m_voiceVolumes[i].store(compPlay->getvoicevolume(i), std::memory_order_relaxed);
            }
            for (int i = 16; i < 20; ++i) {
                m_voiceVolumes[i].store(0, std::memory_order_relaxed);
            }
        } else {
            // VGM 등 기타 포맷의 경우 캐시된 OPL 볼륨값을 비주얼라이저에 바인딩 (OPL3 18채널)
            for (int i = 0; i < 18; ++i) {
                m_voiceVolumes[i].store(m_cachedVoiceVols[i], std::memory_order_relaxed);
            }
            for (int i = 18; i < 20; ++i) {
                m_voiceVolumes[i].store(0, std::memory_order_relaxed);
            }
        }

        int numCh = m_isSop ? 20 : ((m_isRol || m_isIms) ? 16 : 18);
        for (int i = 0; i < numCh; ++i) {
            int chVol = m_voiceVolumes[i].load(std::memory_order_relaxed);
            if (chVol > 0) {
                std::string instNameRaw;
                if (m_isSop) {
                    instNameRaw = static_cast<CsopPlayer*>(m_player)->getvoiceinstrument(i);
                } else if (m_isRol || m_isIms) {
                    instNameRaw = static_cast<CcomposerBackend*>(m_player)->getvoiceinstrument(i);
                } else {
                    instNameRaw = m_cachedVoiceInsts[i];
                }
                if (!instNameRaw.empty()) {
                    QString instName = decodeInstNameCp1361(instNameRaw).trimmed();
                    int inst_idx = m_instruments.indexOf(instName);
                    if (inst_idx >= 0 && inst_idx < 256) {
                        int current_vol = m_instVolumes[inst_idx].load(std::memory_order_relaxed);
                        if (chVol > current_vol) {
                            m_instVolumes[inst_idx].store(chVol, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
    }

    // Precise position update using double to avoid drift
    m_positionRemainder += ((double)framesToRender * 1000.0 / (double)m_sampleRate) * ((double)m_userTempoScale.load() / 100.0);
    if (m_positionRemainder >= 1.0) {
        unsigned long msToAdd = (unsigned long)m_positionRemainder;
        m_position.fetch_add(msToAdd);
        m_positionRemainder -= (double)msToAdd;
    }
}

void ImsPlayer::setUserTempoScale(int scale)
{
    m_userTempoScale.store(scale);
}

int ImsPlayer::getUserTempoScale() const
{
    return m_userTempoScale.load();
}

int ImsPlayer::getCurrentBpm() const
{
    return (int)std::round(m_basicTempo * (m_userTempoScale.load() / 100.0));
}

void ImsPlayer::setUserKeyTranspose(int key)
{
    m_userKeyTranspose.store(qBound(-6, key, 6));
}

int ImsPlayer::getUserKeyTranspose() const
{
    return m_userKeyTranspose.load();
}

void ImsPlayer::forceUpdateOplStereo()
{
    std::lock_guard<std::mutex> lock(m_playerMutex);
    if (m_opl) {
        InterceptingOpl* iopl = static_cast<InterceptingOpl*>(m_opl);
        iopl->CNemuopl::write(0x105, 0x01); // Ensure OPL3 mode is ON
        for (int ch = 0; ch < 18; ++ch) {
            int reg = ((ch >= 9) ? 0x100 : 0) + 0xC0 + (ch % 9);
            int origVal = iopl->originalPanReg[ch];
            int panBit = JJoMeSynth::instance().getChannelPanBit(ch);
            int finalVal = (origVal & ~0x30) | panBit;
            iopl->CNemuopl::write(reg, finalVal);
        }
    }
}



QStringList ImsPlayer::getVoiceInstrumentNames() const
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


