#include "ym2413.h"

#include "emu2413.h"

#include <algorithm>
#include <cstdlib>

Ym2413::Ym2413() = default;

Ym2413::~Ym2413()
{
    if (m_opll) OPLL_delete(m_opll);
}

void Ym2413::init(int clock, int sampleRate)
{
    m_clock = clock > 0 ? clock : 3579545;
    m_sampleRate = sampleRate > 0 ? sampleRate : 44100;
    if (m_opll) { OPLL_delete(m_opll); m_opll = nullptr; }
    m_opll = OPLL_new(uint32_t(m_clock), uint32_t(m_sampleRate));
    if (m_opll) {
        // Quality 1 runs the chip at its own rate and resamples, which is what
        // is wanted here: the device is at 49716 for the OPL3's sake and a VGM
        // names a 3.58 MHz OPLL.
        OPLL_setQuality(m_opll, 1);
        OPLL_reset(m_opll);
    }
    m_rhythm = false;
}

void Ym2413::reset()
{
    if (m_opll) OPLL_reset(m_opll);
    m_rhythm = false;
}

void Ym2413::writeReg(uint8_t reg, uint8_t value)
{
    if (!m_opll) return;
    // 0x0E is the rhythm register; bit 5 swaps channels 7-9 for five drums.
    if (reg == 0x0E) m_rhythm = (value & 0x20) != 0;
    OPLL_writeReg(m_opll, reg, value);
}

void Ym2413::render(int32_t* left, int32_t* right, int frames)
{
    if (!m_opll || frames <= 0) return;
    for (int i = 0; i < frames; ++i) {
        int32_t buf[2] = { 0, 0 };
        OPLL_calcStereo(m_opll, buf);
        // Two of this are emu2413's: it halves every melody voice (_MO) so
        // fourteen fit its own int16 accumulator, and this mixer accumulates
        // in int32, so doubling restores the signal - without touching the
        // melody-to-rhythm balance, which _RO deliberately leaves unhalved.
        //
        // The remaining 1.4 is a level fit. Measured as loudest-5s RMS, these
        // sat at 0.079 against the OPL engines' 0.138 - the first pass matched
        // peaks, which is not what loudness is. 1.4 is what the group's own
        // maximum peak (0.657) allows before clipping.
        left[i]  += int32_t(buf[0] * 2.8);
        right[i] += int32_t(buf[1] * 2.8);
    }
}

int Ym2413::level(int ch) const
{
    if (!m_opll || ch < 0 || ch >= voiceCount()) return 0;
    // ch_out is per-voice, signed and already scaled; the monitor wants a
    // 0-127 loudness, so take the magnitude the same way the other chips do.
    const int v = std::abs(int(m_opll->ch_out[ch]));
    return std::min(127, v / 24);
}
