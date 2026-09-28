#ifndef YM2151_H
#define YM2151_H

#include <cstdint>

// The X68000's YM2151 (OPM), on Nuked-OPM.
//
// This replaces a hand-written emulator. That one needed its phase modulation
// depth "normalised" and its pitch corrected by two octaves before it sounded
// like anything - both of which are signs that the stage before them was not
// the chip. Nuked-OPM is cycle accurate and needs neither.
//
// The interface is the one the sequencer already used, so mxdrv.cpp does not
// care which emulator is underneath.

class Ym2151
{
public:
    Ym2151();
    ~Ym2151();

    // clock is the master clock: 4 MHz on an X68000. rate is the rate the
    // caller wants samples at; the chip runs at clock/64 and is resampled.
    void init(int clock, int rate);
    void reset();

    void writeReg(uint8_t reg, uint8_t data);

    // The sequencer winds through a song with the chip silent to measure its
    // length and to seek. There is no audio to protect there, and queued
    // writes would simply pile up until the queue overflowed and started
    // dropping them - so in that mode they are applied at once instead.
    void setDirectWrites(bool on) { m_directWrites = on; }
    uint8_t readStatus();

    // Adds to the buffer rather than overwriting, so the ADPCM channel can be
    // mixed in by the caller the same way it was before.
    void render(int32_t* left, int32_t* right, int sampleCount);

    bool isTimerAOverflow() const;
    bool isTimerBOverflow() const;
    void resetTimerAOverflow();
    void resetTimerBOverflow();
    void advanceTimers(int samples);

private:
    void clockOne(int32_t* out);      // one chip sample at clock/64

    // Register writes are queued and handed to the chip while it is being
    // clocked for audio, never by clocking it separately. See writeReg().
    void pumpWrites();

    struct PendingWrite { uint8_t reg, data; };
    static const int kPendSize = 4096;
    PendingWrite m_pend[kPendSize];
    int m_pendHead, m_pendTail;
    int m_writeStage;                 // 0 = idle, 1 = address taken
    int m_writeWait;                  // cycles left before the next half
    bool m_directWrites;
    void writeRegNow(uint8_t reg, uint8_t data);

    void* m_chip;   // opm_t, kept opaque: opm.h typedefs an anonymous struct
    int m_clock;
    int m_rate;

    // Resampling from the chip's own rate (clock / 64 - 62500 Hz on an
    // X68000) down to the output rate.
    //
    // This was linear interpolation, and linear interpolation is not a filter.
    // The chip's output carries content up to 31 kHz; decimating to 44.1 kHz
    // without removing it folds 22-31 kHz back down into 13-22 kHz, and that
    // measured **+12 dB** against mxwav's render in the top octave on every
    // song tried - "음색이 다르다", and the reason. X68Sound resamples with a
    // 441-phase, 64-tap polyphase FIR; this is the same idea, with a windowed
    // sinc built here rather than a table copied from there.
    static const int kTaps = 32;
    static const int kPhases = 64;

    void buildFilter();

    float m_fir[kPhases][kTaps];
    int32_t m_hist[2][kTaps];        // newest sample at m_histPos
    int m_histPos;

    float m_tiltA = 0.0f;            // one-pole rolloff coefficient
    float m_tiltZ[2] = {0.0f, 0.0f};
    // DC blocker on the output - see render().
    float m_dcA = 0.0f;
    float m_dcIn[2] = {0.0f, 0.0f};
    float m_dcOut[2] = {0.0f, 0.0f};

    double m_step;                   // chip samples per output sample
    double m_pos;
};

#endif // YM2151_H
