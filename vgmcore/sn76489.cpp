#include "sn76489.h"

#include <cstring>

namespace {

// The attenuation ladder is 2 dB a step, and 0x0F is silence outright rather
// than the next step down.
const int16_t kVolume[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  516,  410,  326,    0,
};

} // namespace

void Sn76489::init(int clock, int sampleRate)
{
    m_clock = (clock > 0) ? clock : 3579545;
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    // How many of the chip's internal (already divided by 16) steps pass in one
    // output sample.
    m_step = double(m_clock) / double(m_divider) / double(m_sampleRate);
    reset();
}

void Sn76489::reset()
{
    for (int i = 0; i < 4; ++i) {
        m_tone[i] = 0;
        m_atten[i] = 0x0F;      // all four channels start silent
        m_counter[i] = 0;
        m_output[i] = 1;
    }
    m_shift = 0x8000;
    m_noiseMode = 0;
    m_latch = 0;
    m_stereo = 0xFF;
    m_fileSetStereo = false;
    m_frac = 0.0;
}

void Sn76489::write(uint8_t value)
{
    if (value & 0x80) {
        // Latch byte: channel, type and the low four bits of the value.
        m_latch = (value >> 4) & 0x07;
        const int ch = (m_latch >> 1) & 3;
        if (m_latch & 1) {
            m_atten[ch] = value & 0x0F;
        } else {
            m_tone[ch] = uint16_t((m_tone[ch] & 0x3F0) | (value & 0x0F));
            if (ch == 3) {
                m_noiseMode = value & 0x07;
                m_shift = 0x8000;
            }
        }
        return;
    }

    // Data byte: the high six bits of whichever tone register was latched.
    const int ch = (m_latch >> 1) & 3;
    if (m_latch & 1) {
        m_atten[ch] = value & 0x0F;
    } else if (ch == 3) {
        m_noiseMode = value & 0x07;
        m_shift = 0x8000;
    } else {
        m_tone[ch] = uint16_t((m_tone[ch] & 0x00F) | ((value & 0x3F) << 4));
    }
}

void Sn76489::clockChip()
{
    for (int ch = 0; ch < 3; ++ch) {
        if (--m_counter[ch] <= 0) {
            m_counter[ch] = m_tone[ch] ? m_tone[ch] : 1;
            m_output[ch] = -m_output[ch];
        }
    }

    // The noise channel's rate is either one of three fixed divisors or
    // whatever channel 2 is set to.
    int period;
    switch (m_noiseMode & 0x03) {
        case 0:  period = 0x10; break;
        case 1:  period = 0x20; break;
        case 2:  period = 0x40; break;
        default: period = m_tone[2] ? m_tone[2] : 1; break;
    }
    if (--m_counter[3] <= 0) {
        m_counter[3] = period;
        // Tapped at bits 0 and 3 for white noise; bit 0 alone gives a tone.
        const uint16_t feedback = (m_noiseMode & 0x04)
            ? uint16_t(((m_shift & 1) ^ ((m_shift >> 3) & 1)) << 15)
            : uint16_t((m_shift & 1) << 15);
        m_shift = uint16_t((m_shift >> 1) | feedback);
        m_output[3] = (m_shift & 1) ? 1 : -1;
    }
}

void Sn76489::render(int32_t* left, int32_t* right, int frames)
{
    if (!left || !right || frames <= 0) return;

    for (int i = 0; i < frames; ++i) {
        // Run the chip forward by one output sample's worth of its own steps.
        m_frac += m_step;
        while (m_frac >= 1.0) {
            clockChip();
            m_frac -= 1.0;
        }

        int l = 0, r = 0;
        for (int ch = 0; ch < 4; ++ch) {
            // A tone period of nought or one is above hearing; the chip holds
            // its output high there rather than buzzing.
            const int level = (ch < 3 && m_tone[ch] < 2)
                ? kVolume[m_atten[ch]]
                : m_output[ch] * kVolume[m_atten[ch]];
            if (m_stereo & (0x10 << ch)) l += level;
            if (m_stereo & (0x01 << ch)) r += level;
        }
        left[i]  += l;
        right[i] += r;
    }
}
