#ifndef VGM_YM2413_H
#define VGM_YM2413_H

#include <cstdint>

struct __OPLL;

/**
 * @brief Yamaha YM2413 (OPLL) - two-operator FM with a fixed instrument ROM.
 *
 * Nine melody channels, or six plus five rhythm voices. Unlike the OPL chips
 * only one instrument is user-defined; the other fifteen live in a ROM inside
 * the chip, which is why an emulator has to carry that table and why this one
 * is not written out by hand the way the PSGs are.
 *
 * Wraps emu2413 (Mitsutaka Okazaki, MIT) - see vgmcore/emu2413-LICENSE.txt.
 * Nuked-OPLL would have been the house choice for accuracy, but it is GPL-2.0
 * rather than the LGPL of the other Nuked cores here, and jmp is MIT-licensed.
 */
class Ym2413
{
public:
    Ym2413();
    ~Ym2413();
    Ym2413(const Ym2413&) = delete;
    Ym2413& operator=(const Ym2413&) = delete;

    void init(int clock, int sampleRate);
    void reset();

    void writeReg(uint8_t reg, uint8_t value);

    // Adds into the buffer, so several chips can share one mix.
    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 0-127 per voice, 9 melody or 6 + 5 rhythm.
    int  level(int ch) const;
    int  voiceCount() const { return m_rhythm ? 11 : 9; }
    bool rhythmMode() const { return m_rhythm; }

private:
    __OPLL* m_opll = nullptr;
    int     m_clock = 3579545;
    int     m_sampleRate = 44100;
    bool    m_rhythm = false;
};

#endif // VGM_YM2413_H
