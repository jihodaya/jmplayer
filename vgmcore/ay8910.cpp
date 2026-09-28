#include "ay8910.h"

#include "emu2149.h"

#include <algorithm>
#include <cstring>

Ay8910::Ay8910() = default;

Ay8910::~Ay8910()
{
    if (m_psg) PSG_delete(m_psg);
}

void Ay8910::init(int clock, int sampleRate, uint8_t chipType, uint8_t flags)
{
    m_clock = clock > 0 ? clock : 1789750;
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    if (m_psg) { PSG_delete(m_psg); m_psg = nullptr; }
    m_psg = PSG_new(uint32_t(m_clock), uint32_t(m_sampleRate));
    if (!m_psg) return;

    PSG_setQuality(m_psg, 1);
    // Volume mode 1 is the AY-3-8910's 16-step table, 2 the YM2149's 32-step.
    PSG_setVolumeMode(m_psg, (chipType >= 0x10) ? 2 : 1);
    if (flags & 0x10) PSG_setClockDivider(m_psg, 1);
    PSG_reset(m_psg);
    std::memset(m_reg, 0, sizeof(m_reg));
}

void Ay8910::reset()
{
    if (m_psg) PSG_reset(m_psg);
    std::memset(m_reg, 0, sizeof(m_reg));
}

void Ay8910::writeReg(uint8_t reg, uint8_t value)
{
    if (!m_psg || reg >= 16) return;
    m_reg[reg] = value;
    PSG_writeReg(m_psg, reg, value);
}

void Ay8910::render(int32_t* left, int32_t* right, int frames)
{
    if (!m_psg || frames <= 0) return;
    for (int i = 0; i < frames; ++i) {
        const int32_t s = PSG_calc(m_psg);
        left[i]  += s;
        right[i] += s;
    }
}

int Ay8910::level(int ch) const
{
    if (ch < 0 || ch > 2) return 0;
    // Registers 8-10 hold each channel's volume; bit 4 says "follow the
    // envelope instead", and there is no per-channel level to read in that
    // case, so report the envelope as fully on.
    const uint8_t v = m_reg[8 + ch];
    // Bits 0-2 of register 7 disable the tone, bits 3-5 the noise; a channel
    // with both off makes no sound whatever its volume says.
    const uint8_t mix = m_reg[7];
    const bool silent = (mix & (1 << ch)) && (mix & (8 << ch));
    if (silent) return 0;
    if (v & 0x10) return 110;
    return int((v & 0x0F) * 127 / 15);
}
