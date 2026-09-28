#ifndef VGM_SN76489_H
#define VGM_SN76489_H

#include <cstdint>

/**
 * @brief Texas Instruments SN76489 PSG (Master System, Mega Drive, Game Gear)
 *
 * Three square channels and one noise channel, each with a 4-bit attenuation.
 * Small enough to write out rather than take a dependency for: the whole chip
 * is four counters, a shift register and a volume table.
 */
class Sn76489
{
public:
    void init(int clock, int sampleRate);
    void reset();

    void write(uint8_t value);
    // Game Gear stereo: one bit per channel per side, high nibble left.
    // A file that writes this has placed its channels itself.
    void writeStereo(uint8_t value) { m_stereo = value; m_fileSetStereo = true; }
    bool fileSetStereo() const { return m_fileSetStereo; }
    void setStereo(uint8_t value) { m_stereo = value; }

    // Adds into the buffer, so several chips can share one mix.
    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 0-127, straight from the attenuation.
    int level(int ch) const {
        if (ch < 0 || ch > 3 || m_atten[ch] >= 0x0F) return 0;
        return 127 - m_atten[ch] * 8;
    }

private:
    int  m_clock = 3579545;
    int  m_sampleRate = 44100;
    int  m_divider = 16;        // the chip's own /16 prescaler
    double m_step = 0.0;        // chip cycles per output sample
    double m_frac = 0.0;

    uint16_t m_tone[4] = {0, 0, 0, 0};
    uint8_t  m_atten[4] = {0x0F, 0x0F, 0x0F, 0x0F};
    int      m_counter[4] = {0, 0, 0, 0};
    int      m_output[4] = {1, 1, 1, 1};

    uint16_t m_shift = 0x8000;
    uint8_t  m_noiseMode = 0;
    uint8_t  m_latch = 0;       // which register the next data byte extends
    uint8_t  m_stereo = 0xFF;
    bool     m_fileSetStereo = false;

    void clockChip();
};

#endif // VGM_SN76489_H
