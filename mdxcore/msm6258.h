#ifndef MSM6258_H
#define MSM6258_H

#include <cstdint>
#include <cstddef>

/**
 * @brief OKI MSM6258 ADPCM 칩 에뮬레이터 (Sharp X68000 사운드)
 *
 * One PCM8 channel: the ADPCM decoder, the rate divider, and the high-pass
 * that X68Sound puts on each channel's output. The low-pass that follows is
 * on the sum of all eight and lives in Mxdrv.
 */
class Msm6258
{
public:
    Msm6258();
    ~Msm6258();

    void init(int outputSampleRate);
    void reset();

    void setSampleRate(int rate);
    // The F command's operand, 0-7: 0-4 are ADPCM at 3.9/5.2/7.8/10.4/15.6
    // kHz, 5 is 16-bit PCM and 6 is 8-bit PCM (both at 15.6 kHz), 7 is off.
    void setFormat(int format);
    void setPan(int pan); // OPM/PCM8 bits: 1 left, 2 right, 3 both
    void setVolume(int vol); // 0-127

    void start(const uint8_t* pcmData, size_t dataSize, bool loop = false);
    void stop();
    bool isPlaying() const { return m_playing; }

    void render(int32_t* left, int32_t* right, int sampleCount);

private:
    int decodeNibble(uint8_t nibble);
    bool nextSample();
    void updateStep();

    int m_outputRate;
    double m_sampleRate;
    int m_format;
    int m_step;
    int m_frac;

    int m_pan;
    int m_volume;
    bool m_playing;
    bool m_loop;

    const uint8_t* m_data;
    size_t m_dataSize;
    size_t m_dataPos;
    bool m_nibbleFlag;

    int32_t m_signal;
    int32_t m_stepIndex;
    int32_t m_pcmPrev;      // the previous raw PCM value, kinds 5 and 6
    int32_t m_lastSample;
    int32_t m_prevSample;   // the one before it, for interpolation

    // The two high-pass stages X68Sound runs on every PCM8 channel.
    float m_hpA, m_hpB;         // pole coefficients at the output rate
    float m_hp1In, m_hp1Out;    // state of the first stage
    float m_hp2In, m_hp2Out;    // and the second
};

/**
 * @brief The X68000's PCM output stage, shared by every engine that plays PDX
 * samples through Msm6258 channels.
 *
 * X68Sound sums its eight PCM8 channels and runs the sum through two low-pass
 * sections at its internal 62.5 kHz before mixing it with the OPM (opm.h
 * pcmset62): a biquad whose poles put it at 3.6 kHz with a Q of 0.71, and a
 * one-pole at 11.8 kHz. That is the machine's own analogue output as the
 * emulator's author measured it, and a good part of why its drums sound the
 * way they do. The same two corners, placed for the device rate by the
 * bilinear transform, then a gain that is measured against mxwav: it is what
 * puts the PCM-only part of a render (full minus FM-only) at the reference's
 * level over the songs that carry a PDX, with the FM already inside 1 dB.
 */
class X68PcmBus
{
public:
    void init(int outputSampleRate);
    void reset();
    // Filters `n` frames of the PCM sum and adds them, scaled, onto the FM.
    void mixInto(const int32_t* pcmL, const int32_t* pcmR,
                 int32_t* outL, int32_t* outR, int n) const;

    static constexpr float kGain = 1.9f;

private:
    float m_b0 = 0, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;   // the biquad
    float m_c0 = 0, m_d1 = 0;                                 // the one-pole
    struct State { float x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, v1 = 0; };
    mutable State m_st[2];
};

#endif // MSM6258_H
