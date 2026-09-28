#ifndef VGM_AY8910_H
#define VGM_AY8910_H

#include <cstdint>

struct __PSG;

/**
 * @brief General Instrument AY-3-8910 / Yamaha YM2149 PSG.
 *
 * Three square channels, a noise source shared between them and one envelope
 * generator, all driven from 16 registers.
 *
 * Wraps emu2149 (Mitsutaka Okazaki, MIT) - see vgmcore/emu2149-LICENSE.txt.
 * The SN76489 next door is written out by hand, and this chip is barely harder,
 * but the two parts that decide whether it sounds right - the sixteen envelope
 * shapes and the logarithmic volume table, which differs between the AY and the
 * YM - are exactly the parts a hand-written one gets subtly wrong.
 */
class Ay8910
{
public:
    Ay8910();
    ~Ay8910();
    Ay8910(const Ay8910&) = delete;
    Ay8910& operator=(const Ay8910&) = delete;

    // chipType and flags come from the VGM header at 0x78 and 0x79: type 0x10
    // and up is a YM2149, whose volume table is finer, and flag bit 0x10 is
    // the YM's clock-divider pin held low.
    void init(int clock, int sampleRate, uint8_t chipType = 0, uint8_t flags = 0);
    void reset();

    void writeReg(uint8_t reg, uint8_t value);

    // Adds into the buffer, so several chips can share one mix. The chip is
    // mono; both sides get the same sample.
    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 0-127 from the channel's own volume, taking the
    // envelope into account when the channel is following it.
    int level(int ch) const;

private:
    __PSG*  m_psg = nullptr;
    int     m_clock = 1789750;
    int     m_sampleRate = 44100;
    uint8_t m_reg[16] = { 0 };
};

#endif // VGM_AY8910_H
