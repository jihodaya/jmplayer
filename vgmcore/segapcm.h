#ifndef VGM_SEGAPCM_H
#define VGM_SEGAPCM_H

#include <cstdint>
#include <vector>

/**
 * @brief Sega 315-5218 "SegaPCM" - 16 channels of 8-bit sample playback.
 *
 * The OutRun / After Burner / Space Harrier boards pair a YM2151 with this:
 * the FM chip plays the music and this one plays the drums, the engine and the
 * voices. A VGM that names both and gets only the FM sounds thin and has no
 * percussion at all, which is exactly how those files played here.
 *
 * Small enough to write out rather than depend on. Each channel owns eight
 * bytes in each half of a 256-byte register file:
 *
 *   0x02 + ch*8   left volume        0x03 + ch*8   right volume
 *   0x04 + ch*8   loop address low   0x05 + ch*8   loop address high
 *   0x06 + ch*8   end address high   0x07 + ch*8   pitch (address step)
 *   0x84 + ch*8   address low        0x85 + ch*8   address high
 *   0x86 + ch*8   bit 0 off, bit 1 no loop, bits 4-6 ROM bank
 *
 * Sample 0x80 is silence.
 */
class SegaPcm
{
public:
    void init(int clock, int sampleRate);
    void reset();

    // The VGM header's SegaPCM interface word (offset 0x3C) says how the flags
    // register's upper bits pick a ROM bank. Without it every channel reads
    // from the first 64 KB and a song whose samples live higher up is silent.
    void setInterface(uint32_t intf);

    // The sample ROM arrives as a VGM data block of type 0x80, in pieces.
    void setRom(const uint8_t* data, size_t size, size_t offset, size_t totalSize);

    void write(uint16_t addr, uint8_t value);

    // Adds into the buffer, so several chips can share one mix.
    void render(int32_t* left, int32_t* right, int frames);

    // For the channel monitor: 0-127 from the channel's own volume registers.
    int level(int ch) const;

private:
    std::vector<uint8_t> m_rom;
    uint8_t  m_reg[0x100] = { 0 };
    uint8_t  m_low[16] = { 0 };     // the address's fractional byte, as on the die
    int      m_clock = 4000000;
    int      m_sampleRate = 44100;
    double   m_step = 1.0;          // chip samples per output frame
    double   m_frac = 0.0;
    int32_t  m_heldL = 0;           // the sample the chip is holding between
    int32_t  m_heldR = 0;           // its own ticks, which are slower than ours

    uint32_t m_intf = 0;
    int      m_bankShift = 12;
    uint32_t m_bankMask = 0x70;
    uint32_t m_romMask = 0;

    void recomputeBanking();
    void clockChip(int32_t& l, int32_t& r);
};

#endif // VGM_SEGAPCM_H
