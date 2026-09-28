#ifndef VGM_YM2610_H
#define VGM_YM2610_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief Yamaha YM2610 (OPNB) - the Neo Geo's sound chip.
 *
 * Four FM channels, three SSG squares, six ADPCM-A percussion channels and one
 * ADPCM-B sample channel, all in one part. That is four separate synthesis
 * models, which is why this is the one chip here that is not written out by
 * hand: a VGM's two sample ROMs arrive as data blocks and the chip reads them
 * back through a callback while it plays.
 *
 * Wraps ymfm (Aaron Giles, BSD-3-Clause) - see vgmcore/ymfm/LICENSE.
 */
class Ym2610
{
public:
    Ym2610();
    ~Ym2610();
    Ym2610(const Ym2610&) = delete;
    Ym2610& operator=(const Ym2610&) = delete;

    void init(int clock, int sampleRate);
    void reset();

    // VGM data block 0x82 is the ADPCM-A ROM and 0x83 the ADPCM-B (delta-T)
    // one; the chip reads both by address while it plays.
    void setAdpcmRomA(const uint8_t* data, size_t size, size_t offset, size_t totalSize);
    void setAdpcmRomB(const uint8_t* data, size_t size, size_t offset, size_t totalSize);

    // port 0 is command 0x58, port 1 is command 0x59.
    void write(int port, uint8_t reg, uint8_t value);

    // Adds into the buffer, so several chips can share one mix.
    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 4 FM, then 3 SSG, then 6 ADPCM-A, then ADPCM-B.
    static const int kVoices = 14;
    int level(int ch) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // VGM_YM2610_H
