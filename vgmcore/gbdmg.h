#ifndef VGM_GBDMG_H
#define VGM_GBDMG_H

#include <cstdint>

/**
 * @brief Game Boy (DMG) sound hardware.
 *
 * Four channels: two square waves with a volume envelope (the first also has a
 * frequency sweep), a 32-step 4-bit wavetable, and a noise generator built from
 * a shift register. Registers are 0xFF10-0xFF26 with the wave memory at
 * 0xFF30-0xFF3F, and a VGM addresses them by their offset from 0xFF10.
 *
 * Written out rather than depended on: the whole chip is a few counters, and
 * its behaviour is documented to the cycle.
 */
class GbDmg
{
public:
    void init(int clock, int sampleRate);
    void reset();

    // reg is the offset from 0xFF10, so 0x00 is NR10 and 0x20-0x2F is wave RAM.
    void write(uint8_t reg, uint8_t value);

    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 0-127 while the channel is sounding.
    int level(int ch) const;

    // NR51 as the file wrote it, and a way to put a different one in.
    uint8_t routing() const { return m_nr51; }
    void setRouting(uint8_t v) { m_nr51 = v; }

private:
    struct Square {
        uint16_t freq = 0;
        uint8_t  duty = 2;
        int      timer = 1;
        int      phase = 0;
        bool     enabled = false;

        uint8_t  envInit = 0, envPeriod = 0;
        bool     envUp = false;
        uint8_t  volume = 0;
        int      envTimer = 0;

        int      length = 0;
        bool     lengthEnabled = false;
    };

    struct Wave {
        uint16_t freq = 0;
        int      timer = 1;
        int      phase = 0;
        bool     dacOn = false;
        bool     enabled = false;
        uint8_t  shift = 0;          // 0 = mute, 1 = full, 2 = half, 3 = quarter
        int      length = 0;
        bool     lengthEnabled = false;
        uint8_t  ram[16] = {0};
    };

    struct Noise {
        uint16_t lfsr = 0x7FFF;
        uint8_t  divisor = 0, shift = 0;
        bool     narrow = false;
        int      timer = 1;
        bool     enabled = false;

        uint8_t  envInit = 0, envPeriod = 0;
        bool     envUp = false;
        uint8_t  volume = 0;
        int      envTimer = 0;

        int      length = 0;
        bool     lengthEnabled = false;
    };

    void stepChip();                 // one master-clock tick's worth of work
    void stepFrameSequencer();
    void clockEnvelope(uint8_t& vol, int& timer, uint8_t period, bool up);
    void clockSweep();

    int m_clock = 4194304;
    int m_sampleRate = 44100;
    double m_step = 0.0;             // chip cycles per output sample
    double m_frac = 0.0;

    Square m_sq[2];
    Wave   m_wave;
    Noise  m_noise;

    // Channel 1's frequency sweep.
    uint8_t m_sweepPeriod = 0, m_sweepShift = 0;
    bool    m_sweepDown = false;
    int     m_sweepTimer = 0;
    uint16_t m_sweepShadow = 0;
    bool    m_sweepOn = false;

    uint8_t m_nr50 = 0x77;           // master volume, left and right
    uint8_t m_nr51 = 0xFF;           // which channel reaches which side
    bool    m_powered = true;

    int m_seqTimer = 0;              // 8192 cycles = 512 Hz
    int m_seqStep = 0;
};

#endif // VGM_GBDMG_H
