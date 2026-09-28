#include "segapcm.h"

#include <algorithm>
#include <cstring>

namespace {

// The chip produces one sample every 128 clocks - 31250 Hz on OutRun's 4 MHz.
const int kClockDivider = 128;

// A sample is signed 8-bit and each volume register is 0-127, so one channel
// at full tilt is +-16256 and sixteen of them would be twenty times over the
// mixer's 16-bit scale. Halving leaves a single channel a quarter of full
// scale, which measures the same order as the YM2151 beside it on the OutRun
// files - the two are meant to balance.
inline int32_t scaled(int v, int vol) { return int32_t(v * vol) >> 1; }

} // namespace

void SegaPcm::init(int clock, int sampleRate)
{
    m_clock = clock > 0 ? clock : 4000000;
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    m_step = double(m_clock) / double(kClockDivider) / double(m_sampleRate);
    m_rom.clear();
    m_romMask = 0;
    reset();
}

void SegaPcm::reset()
{
    std::memset(m_reg, 0, sizeof(m_reg));
    std::memset(m_low, 0, sizeof(m_low));
    // Bit 0 of the flags register means "channel off", and a silent chip is
    // the right state to start from.
    for (int i = 0; i < 16; ++i) m_reg[0x86 + i * 8] = 0x01;
    m_frac = 0.0;
    m_heldL = m_heldR = 0;
}

void SegaPcm::setInterface(uint32_t intf)
{
    m_intf = intf;
    recomputeBanking();
}

void SegaPcm::recomputeBanking()
{
    // The low nibble is how far the bank bits shift; the high word is which
    // flag bits are bank bits, and zero there means the common 0x70.
    m_bankShift = int(m_intf & 0x0F);
    uint32_t mask = m_intf >> 16;
    if (mask == 0) mask = 0x70;
    m_bankMask = mask & (m_romMask >> m_bankShift);
    if (m_bankMask == 0) m_bankMask = mask;
}

void SegaPcm::setRom(const uint8_t* data, size_t size, size_t offset, size_t totalSize)
{
    if (!data || size == 0) return;
    // 0x80 is silence, so a ROM with holes in it is quiet rather than buzzing.
    const size_t want = std::max(totalSize, offset + size);
    if (m_rom.size() < want) m_rom.resize(want, 0x80);
    std::memcpy(m_rom.data() + offset, data, size);

    // The mask the banking uses is the ROM's size rounded up to a power of two,
    // which is what a real board's address decoding gives.
    uint32_t m = 1;
    while (m < uint32_t(m_rom.size())) m <<= 1;
    m_romMask = m - 1;
    recomputeBanking();
}

void SegaPcm::write(uint16_t addr, uint8_t value)
{
    const uint16_t a = addr & 0xFF;
    // Clearing bit 0 of the flags register starts the channel, and it starts
    // from the address registers the song has just written.
    if ((a & 0x87) == 0x86 && (m_reg[a] & 0x01) && !(value & 0x01))
        m_low[(a >> 3) & 0x0F] = 0;
    m_reg[a] = value;
}

void SegaPcm::clockChip(int32_t& outL, int32_t& outR)
{
    int32_t l = 0, r = 0;
    for (int ch = 0; ch < 16; ++ch) {
        uint8_t* reg = m_reg + ch * 8;
        if (reg[0x86] & 0x01) { m_low[ch] = 0; continue; }      // channel off

        const uint32_t bank = (uint32_t(reg[0x86]) & m_bankMask) << m_bankShift;
        const uint8_t  end  = uint8_t(reg[0x06] + 1);
        const uint32_t loop = (uint32_t(reg[0x05]) << 16) | (uint32_t(reg[0x04]) << 8);
        uint32_t addr = (uint32_t(reg[0x85]) << 16) | (uint32_t(reg[0x84]) << 8) | m_low[ch];

        if (uint8_t(addr >> 16) == end) {
            // Bit 1 of the flags disables looping, and the channel stops.
            if (reg[0x86] & 0x02) { reg[0x86] |= 0x01; m_low[ch] = 0; continue; }
            addr = loop;
        }

        const size_t idx = size_t(bank) + size_t((addr >> 8) & 0xFFFF);
        const int v = (idx < m_rom.size()) ? int(m_rom[idx]) - 0x80 : 0;
        l += scaled(v, reg[0x02] & 0x7F);
        r += scaled(v, reg[0x03] & 0x7F);

        addr = (addr + reg[0x07]) & 0xFFFFFF;
        reg[0x84] = uint8_t(addr >> 8);
        reg[0x85] = uint8_t(addr >> 16);
        m_low[ch] = uint8_t(addr);
    }
    outL = l;
    outR = r;
}

void SegaPcm::render(int32_t* left, int32_t* right, int frames)
{
    if (!left || !right || frames <= 0 || m_rom.empty()) return;

    for (int i = 0; i < frames; ++i) {
        // The chip is slower than the device, so most output frames repeat the
        // sample the chip is holding - the hardware has no filter either.
        m_frac += m_step;
        while (m_frac >= 1.0) {
            clockChip(m_heldL, m_heldR);
            m_frac -= 1.0;
        }
        left[i]  += m_heldL;
        right[i] += m_heldR;
    }
}

int SegaPcm::level(int ch) const
{
    if (ch < 0 || ch > 15) return 0;
    if (m_reg[0x86 + ch * 8] & 0x01) return 0;
    const int vl = m_reg[0x02 + ch * 8] & 0x7F;
    const int vr = m_reg[0x03 + ch * 8] & 0x7F;
    // The register holds seven bits but the music uses six: across all 13
    // SegaPCM files here the value never once exceeds 63, so treating 127 as
    // full scale left these bars at half height beside the FM ones.
    return std::min(127, std::max(vl, vr) * 127 / 63);
}
