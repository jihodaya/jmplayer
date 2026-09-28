#include "ym2612.h"

#include "ym3438.h"

#include <cstdlib>
#include <cstring>

Ym2612::Ym2612()
{
    m_chip = std::calloc(1, sizeof(ym3438_t));
}

Ym2612::~Ym2612()
{
    std::free(m_chip);
    m_chip = nullptr;
}

void Ym2612::init(int clock, int rate)
{
    m_clock = (clock > 0) ? clock : 7670448;
    m_rate = (rate > 0) ? rate : 44100;

    // The DAC is multiplexed over 24 cycles, six channels at four cycles each,
    // and one cycle is six master clocks - so a finished sample every 144.
    const double native = double(m_clock) / 144.0;
    m_step = native / double(m_rate);
    reset();
}

void Ym2612::reset()
{
    if (!m_chip) return;
    ym3438_t* chip = (ym3438_t*)m_chip;
    // ym3438_mode_ym2612 selects the discrete YM2612 rather than the later
    // integrated YM3438: nine-bit DAC with its ladder effect, which is the
    // sound a Mega Drive recording was made against.
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(chip);
    m_queue.clear();
    m_pos = 0.0;
    m_prev[0] = m_prev[1] = 0;
    m_next[0] = m_next[1] = 0;
}

void Ym2612::write(uint8_t port, uint8_t data)
{
    m_queue.push_back(uint16_t((uint16_t(port) << 8) | data));
}

// One sample at the chip's own rate: twenty-four cycles, summed, because the
// real chip's output is that multiplexed stream through an external filter.
void Ym2612::clockNative(int32_t* out)
{
    ym3438_t* chip = (ym3438_t*)m_chip;
    int32_t l = 0, r = 0;
    for (int i = 0; i < 24; ++i) {
        if (!m_queue.empty() && !chip->write_busy) {
            const uint16_t w = m_queue.front();
            m_queue.pop_front();
            OPN2_Write(chip, w >> 8, uint8_t(w & 0xFF));
        }
        Bit16s buf[2] = {0, 0};
        OPN2_Clock(chip, buf);
        l += buf[0];
        r += buf[1];
    }
    out[0] = l;
    out[1] = r;
}

void Ym2612::drain()
{
    if (!m_chip) return;
    ym3438_t* chip = (ym3438_t*)m_chip;
    int guard = 0;
    while (!m_queue.empty() && ++guard < 100000) {
        if (!chip->write_busy) {
            const uint16_t w = m_queue.front();
            m_queue.pop_front();
            OPN2_Write(chip, w >> 8, uint8_t(w & 0xFF));
        }
        Bit16s buf[2] = {0, 0};
        OPN2_Clock(chip, buf);
    }
}

void Ym2612::render(int32_t* left, int32_t* right, int frames)
{
    if (!m_chip || !left || !right || frames <= 0) return;

    for (int i = 0; i < frames; ++i) {
        m_pos += m_step;
        while (m_pos >= 1.0) {
            m_prev[0] = m_next[0];
            m_prev[1] = m_next[1];
            clockNative(m_next);
            m_pos -= 1.0;
        }
        const double f = m_pos;
        left[i]  += int32_t(m_prev[0] + (m_next[0] - m_prev[0]) * f);
        right[i] += int32_t(m_prev[1] + (m_next[1] - m_prev[1]) * f);
    }
}
