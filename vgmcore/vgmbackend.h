#ifndef VGM_VGMBACKEND_H
#define VGM_VGMBACKEND_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "sn76489.h"
#include "gbdmg.h"
#include "ay8910.h"
#include "ym2413.h"
#include "segapcm.h"
#include "ym2610.h"
#include "qsound.h"

class Ym2151;
class Ym2612;

/**
 * @brief Video Game Music (.vgm / .vgz) playback.
 *
 * A VGM is a log of chip register writes with waits between them, so the player
 * is a command interpreter rather than a sequencer: no tempo, no tracks, just
 * "write this, then let the chips run for n samples".
 *
 * The chips handled here are the ones this library actually asks for. Anything
 * else is parsed for its length and skipped, which keeps the stream in step -
 * the same discipline the MDX engine needed.
 */
class VgmBackend
{
public:
    VgmBackend();
    ~VgmBackend();

    void init(int sampleRate);
    void reset();

    bool load(const uint8_t* data, size_t size);

    // True when the file wants nothing but OPL chips, which AdPlug already
    // plays; the caller can leave those where they were.
    static bool wantsOnlyOpl(const uint8_t* data, size_t size);
    // Every chip the file's header declares a clock for, whether this build
    // plays it or not, as "YM2610, QSound (not played)". A file that names
    // nothing this build has is refused rather than played as silence, so this
    // is what lets the caller say WHY instead of "failed to load".
    static std::string describeChips(const uint8_t* data, size_t size);
    // Unpacks a .vgz (or a gzipped .vgm, which is common) in place.
    static bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

    void play()  { m_playing = true; m_paused = false; }
    void pause() { m_paused = true; }
    void stop()  { m_playing = false; m_paused = false; reset(); }
    bool isPlaying() const { return m_playing && !m_paused; }

    void setVolume(float v) { m_volume = v; }
    // jmp's F12 virtual-stereo pattern, 1 (off) to 9.
    void setStereoMode(int mode);
    void setMaxLoops(int n) { m_maxLoops = n; }

    void render(float* buffer, int frameCount);

    uint32_t getElapsedMs() const;
    uint32_t getTotalMs() const { return m_totalMs; }
    void seekMs(uint32_t ms);

    const std::string& title() const { return m_title; }
    const std::string& game()  const { return m_game; }
    const std::string& chips() const { return m_chips; }

    // Channel monitor: one entry per chip voice, in the order chipNames()
    // lists them. Levels are what the chips are being told, not a meter on the
    // output - the same basis the OPL and MDX monitors use.
    int voiceCount() const;
    int voiceLevel(int i) const;
    std::string voiceName(int i) const;

    // Parses the whole command stream without producing audio and reports
    // whether it ended where the header says it should. See the .cpp.
    struct Check {
        bool  reachedEnd = false;
        bool  samplesMatch = false;
        long  declaredSamples = 0;
        long  countedSamples = 0;
        long  unknownCommands = 0;
        uint8_t firstUnknown = 0;
    };
    Check verify() const;

private:
    void writeChip(uint8_t cmd, uint8_t a, uint8_t b);
    void runCommands();                 // up to the next wait
    void addWait(int vgmSamples);
    void drainChipWrites();             // for seeking: apply, do not sound
    void renderChips(int32_t* l, int32_t* r, int frames);
    void handleDataBlock();
    void applyPanOverrides();
    char patternFor(int voice) const;   // 'L', 'R' or 'M'
    uint8_t pannedOpm(int ch) const;
    uint8_t pannedOpn2(int ch) const;
    int opmVoiceBase() const;
    int  commandLength(uint8_t cmd, size_t pos) const;

    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
    size_t m_dataStart = 0;
    size_t m_loopStart = 0;
    bool   m_hasLoop = false;

    int m_sampleRate = 44100;
    long long m_samplePos = 0;
    // How many OUTPUT frames the current wait still has to run for.
    //
    // This used to be the VGM's own 44.1 kHz sample count, converted to frames
    // with integer arithmetic on every chunk. The device runs at 49716, so a
    // one-sample wait asked for one frame and then subtracted
    // 1 * 44100 / 49716 = 0 from itself - the wait never expired and the song
    // stopped dead after its first short one. Measured, that took the peak of
    // a Mega Drive song from 0.10 to 0.004.
    double m_waitOut = 0.0;
    int m_loopCount = 0;
    int m_maxLoops = 2;
    bool m_playing = false;
    bool m_paused = false;
    bool m_ended = false;
    float m_volume = 1.0f;

    uint32_t m_version = 0;
    uint32_t m_totalMs = 0;
    long m_declaredSamples = 0;

    std::string m_title, m_game, m_chips;

    // Chips. Only those the file declares a clock for are stepped.
    bool m_hasPsg = false, m_hasOpn2 = false, m_hasOpm = false, m_hasDmg = false;
    bool m_hasOpll = false, m_hasAy = false, m_hasSpcm = false, m_hasOpnb = false;
    bool m_hasQsound = false;
    Sn76489  m_psg;
    GbDmg    m_dmg;
    Ay8910   m_ay;
    Ym2413   m_opll;
    SegaPcm  m_spcm;
    Ym2610   m_opnb;
    Qsound   m_qsound;
    Ym2612*   m_opn2 = nullptr;
    Ym2151*   m_opm = nullptr;
    int m_opn2Clock = 0, m_opmClock = 0;

    // Stereo placement as the FILE wrote it, so an override can tell a channel
    // the song centred from one it deliberately placed.
    int m_stereoMode = 1;
    uint8_t m_opn2Pan[6] = {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0};   // reg 0xB4
    uint8_t m_opmPan[8]  = {0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0};
    uint8_t m_opmFbAlg[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    // Key-on state per FM channel, for the monitor. The FM chips do not carry
    // a level anywhere readable, so this follows the key-on and key-off writes
    // the song makes and decays afterwards.
    uint8_t m_fmLevel[16] = {0};
    long long m_fmLastOn[16] = {0};

    // The YM2612's DAC is fed from a data block rather than from registers.
    std::vector<uint8_t> m_pcmBank;
    size_t m_pcmPos = 0;

    // The OPLL has no readable level either, so its key-ons are followed the
    // same way the other FM chips' are.
    uint8_t m_opllLevel[11] = {0};
    long long m_opllLastOn[11] = {0};
};

#endif // VGM_VGMBACKEND_H
