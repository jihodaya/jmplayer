#ifndef MLDFM_H
#define MLDFM_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include "mdxcore/ym2151.h"
#include "mdxcore/msm6258.h"

// The OPM (and ADPCM) half of an MLD `.mdz`.
//
// Twelve of the 158 songs here route some tracks to a MIDI module and the rest
// to the X68000's own YM2151, and their titles say so - "For SC55+X680x0",
// "for CM-64+PCM8", and one that reads "SC-55+OPM+ADPCM". Played through the
// MIDI path alone they lose 31% of their notes.
//
// **This shares its byte walk with mdxmidi.cpp**, because an FM track and a
// MIDI track are the SAME command stream: `CH1 c4` compiles to `E1 00 3C 30`
// and `C1 c4` to `E1 10 3C 30`, and `@`, `@v`, `V`, `p` and `@q` all reach the
// same opcodes with the same arithmetic. Only the destination differs, which is
// why this is an output backend rather than a second sequencer.
//
// What it cannot claim is the MIDI half's evidence. `mdz2mus.x` decompiles an
// FM track just as happily, but that says what the file contains, not how the
// chip is driven - and `mld.x` is a resident X68000 driver, so run68 cannot run
// it and there is no `mxwav` equivalent to render against. The voice record
// below IS measured (compiled with mlc.x and read back); the key code, the TL
// arithmetic and the volume curve are taken from the MXDRV engine next door,
// which was scored against a real reference on the same chip. Those parts have
// to be judged by ear.
class MldFm
{
public:
    MldFm();

    void init(int sampleRate);
    // Returns false when the file has no OPM or PCM tracks at all.
    bool load(const uint8_t* data, size_t size,
              const uint8_t* pdx = nullptr, size_t pdxSize = 0);

    void play();
    void stop();
    bool isPlaying() const { return m_playing; }

    // Wind to `ms` of SONG time (unscaled by the tempo hotkeys) with the chip
    // silent, the way Mxdrv::seekMs does. The MIDI half seeks in the same
    // units, so the two halves meet again afterwards.
    void seekMs(unsigned long ms);
    unsigned long positionMs() const;

    // F7/F8 and F9/F10. The MIDI half applies the same two numbers to its own
    // clock and notes; without these the FM half drifted off it at once.
    void setTempoScale(int percent);
    void setTranspose(int semitones);

    // Stereo, interleaved.
    void render16(int16_t* out, int frames);

    void setVolume(float v) { m_volume = v; }
    int  trackCount() const { return int(m_tracks.size()); }
    long keyOnCount() const { return m_keyOns; }

private:
    struct Track {
        int start = 0, end = 0;      // byte range in the file
        int p = 0;                   // read position
        int ch = -1;                 // 0-7 OPM, 8+ PCM
        int wait = 0;                // ticks left before the next event
        int vel = 106;               // @v as loudness 0-127; v8 is the default
        int velByte = 0x08;          // the raw 0xFB operand, which the ADPCM wants
        int voiceVol = 0;            // the voice record's own VOL, an attenuation
        int vol = 127;               // V
        int gateQ = 0;               // the 0xF8 operand, 0 = none
        int gateLeft = -1;           // ticks until key off, -1 = not sounding
        int voice = -1;
        int pan = 3;                 // OPM stereo bits, both sides
        bool tie = false;
        bool active = false;
        bool audible = true;   // false for a MIDI track, walked for its tempo
        std::vector<int> loop;       // plays left, per open loop
        long ticks = 0;              // clocks consumed, for MLD_KEYLOG
        int opTL[4] = {0, 0, 0, 0};
        int slotMask = 0x0F;
        int pmsAms = 0;              // MH's last operand, for MHON
        int alg = 0;
        int fbAlg = 0;
    };

    void reset();
    void setupTracks();
    void stepTick();
    void runTrack(Track& t);
    void sendVoice(Track& t, int voiceIdx);
    void applyVolume(Track& t);
    void keyOn(Track& t, int note);
    void keyOff(Track& t);
    const uint8_t* findVoice(int number) const;
    int  signed16At(int p) const;

    std::vector<uint8_t> m_data;
    std::vector<uint8_t> m_pdx;
    int m_base = 0;
    int m_voiceOffset = 0;

    std::vector<Track> m_tracks;
    Ym2151 m_opm;
    static const int kPcmVoices = 8;
    Msm6258 m_pcm[kPcmVoices];
    X68PcmBus m_pcmBus;     // the X68000's PCM output filter and level

    int m_sampleRate = 44100;
    int m_timerB = 200;
    double m_samplesPerTick = 1000.0;  // exact; the fraction is carried below
    double m_tickAcc = 0.0;
    int m_tickCounter = 0;
    bool m_playing = false;
    float m_volume = 1.0f;
    long m_keyOns = 0;
    long m_samplePos = 0;   // frames rendered since play(), for the key-on log
    double m_songSamples = 0.0;  // song time in samples, tempo scale NOT applied
    int m_tempoScale = 100;
    int m_transpose = 0;
};

#endif
