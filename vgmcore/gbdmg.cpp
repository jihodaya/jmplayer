#include "gbdmg.h"

#include <cstring>

namespace {

// Four duty cycles, eight steps each: 12.5, 25, 50 and 75 per cent.
const uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};

// The noise channel's base divisor. Entry 0 is a half-step, not a zero.
const int kNoiseDivisor[8] = {8, 16, 32, 48, 64, 80, 96, 112};

} // namespace


void GbDmg::init(int clock, int sampleRate)
{
    m_clock = (clock > 0) ? clock : 4194304;
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    m_step = double(m_clock) / double(m_sampleRate);
    reset();
}

void GbDmg::reset()
{
    m_sq[0] = Square();
    m_sq[1] = Square();
    m_wave = Wave();
    m_noise = Noise();
    m_sweepPeriod = m_sweepShift = 0;
    m_sweepDown = false;
    m_sweepTimer = 0;
    m_sweepShadow = 0;
    m_sweepOn = false;
    m_nr50 = 0x77;
    m_nr51 = 0xFF;
    m_powered = true;
    m_seqTimer = 0;
    m_seqStep = 0;
    m_frac = 0.0;
}

void GbDmg::write(uint8_t reg, uint8_t value)
{
    // Wave memory sits apart from the registers.
    if (reg >= 0x20 && reg <= 0x2F) {
        m_wave.ram[reg - 0x20] = value;
        return;
    }

    switch (reg) {
        // ---- channel 1: square with sweep
        case 0x00:
            m_sweepPeriod = (value >> 4) & 0x07;
            m_sweepDown = (value & 0x08) != 0;
            m_sweepShift = value & 0x07;
            break;
        case 0x01:
            m_sq[0].duty = (value >> 6) & 3;
            m_sq[0].length = 64 - (value & 0x3F);
            break;
        case 0x02:
            m_sq[0].envInit = (value >> 4) & 0x0F;
            m_sq[0].envUp = (value & 0x08) != 0;
            m_sq[0].envPeriod = value & 0x07;
            // Clearing the top five bits turns the channel's DAC off outright.
            if ((value & 0xF8) == 0) m_sq[0].enabled = false;
            break;
        case 0x03:
            m_sq[0].freq = uint16_t((m_sq[0].freq & 0x700) | value);
            break;
        case 0x04:
            m_sq[0].freq = uint16_t((m_sq[0].freq & 0x0FF) | ((value & 0x07) << 8));
            m_sq[0].lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80) {
                m_sq[0].enabled = (m_sq[0].envInit != 0) || m_sq[0].envUp;
                m_sq[0].volume = m_sq[0].envInit;
                m_sq[0].envTimer = m_sq[0].envPeriod ? m_sq[0].envPeriod : 8;
                m_sq[0].timer = (2048 - m_sq[0].freq) * 4;
                if (m_sq[0].length == 0) m_sq[0].length = 64;
                m_sweepShadow = m_sq[0].freq;
                m_sweepTimer = m_sweepPeriod ? m_sweepPeriod : 8;
                m_sweepOn = (m_sweepPeriod != 0) || (m_sweepShift != 0);
            }
            break;

        // ---- channel 2: square
        case 0x06:
            m_sq[1].duty = (value >> 6) & 3;
            m_sq[1].length = 64 - (value & 0x3F);
            break;
        case 0x07:
            m_sq[1].envInit = (value >> 4) & 0x0F;
            m_sq[1].envUp = (value & 0x08) != 0;
            m_sq[1].envPeriod = value & 0x07;
            if ((value & 0xF8) == 0) m_sq[1].enabled = false;
            break;
        case 0x08:
            m_sq[1].freq = uint16_t((m_sq[1].freq & 0x700) | value);
            break;
        case 0x09:
            m_sq[1].freq = uint16_t((m_sq[1].freq & 0x0FF) | ((value & 0x07) << 8));
            m_sq[1].lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80) {
                m_sq[1].enabled = (m_sq[1].envInit != 0) || m_sq[1].envUp;
                m_sq[1].volume = m_sq[1].envInit;
                m_sq[1].envTimer = m_sq[1].envPeriod ? m_sq[1].envPeriod : 8;
                m_sq[1].timer = (2048 - m_sq[1].freq) * 4;
                if (m_sq[1].length == 0) m_sq[1].length = 64;
            }
            break;

        // ---- channel 3: wavetable
        case 0x0A:
            m_wave.dacOn = (value & 0x80) != 0;
            if (!m_wave.dacOn) m_wave.enabled = false;
            break;
        case 0x0B:
            m_wave.length = 256 - value;
            break;
        case 0x0C:
            m_wave.shift = (value >> 5) & 3;
            break;
        case 0x0D:
            m_wave.freq = uint16_t((m_wave.freq & 0x700) | value);
            break;
        case 0x0E:
            m_wave.freq = uint16_t((m_wave.freq & 0x0FF) | ((value & 0x07) << 8));
            m_wave.lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80) {
                m_wave.enabled = m_wave.dacOn;
                m_wave.phase = 0;
                m_wave.timer = (2048 - m_wave.freq) * 2;
                if (m_wave.length == 0) m_wave.length = 256;
            }
            break;

        // ---- channel 4: noise
        case 0x10:
            m_noise.length = 64 - (value & 0x3F);
            break;
        case 0x11:
            m_noise.envInit = (value >> 4) & 0x0F;
            m_noise.envUp = (value & 0x08) != 0;
            m_noise.envPeriod = value & 0x07;
            if ((value & 0xF8) == 0) m_noise.enabled = false;
            break;
        case 0x12:
            m_noise.shift = (value >> 4) & 0x0F;
            m_noise.narrow = (value & 0x08) != 0;
            m_noise.divisor = value & 0x07;
            break;
        case 0x13:
            m_noise.lengthEnabled = (value & 0x40) != 0;
            if (value & 0x80) {
                m_noise.enabled = (m_noise.envInit != 0) || m_noise.envUp;
                m_noise.volume = m_noise.envInit;
                m_noise.envTimer = m_noise.envPeriod ? m_noise.envPeriod : 8;
                m_noise.lfsr = 0x7FFF;
                m_noise.timer = kNoiseDivisor[m_noise.divisor] << m_noise.shift;
                if (m_noise.length == 0) m_noise.length = 64;
            }
            break;

        // ---- control
        case 0x14: m_nr50 = value; break;
        case 0x15: m_nr51 = value; break;
        case 0x16:
            m_powered = (value & 0x80) != 0;
            if (!m_powered) {
                m_sq[0].enabled = m_sq[1].enabled = false;
                m_wave.enabled = m_noise.enabled = false;
            }
            break;
        default: break;
    }
}

void GbDmg::clockEnvelope(uint8_t& vol, int& timer, uint8_t period, bool up)
{
    if (period == 0) return;
    if (--timer > 0) return;
    timer = period;
    if (up && vol < 15) ++vol;
    else if (!up && vol > 0) --vol;
}

void GbDmg::clockSweep()
{
    if (!m_sweepOn) return;
    if (--m_sweepTimer > 0) return;
    m_sweepTimer = m_sweepPeriod ? m_sweepPeriod : 8;
    if (m_sweepPeriod == 0) return;

    const uint16_t delta = uint16_t(m_sweepShadow >> m_sweepShift);
    const int next = m_sweepDown ? int(m_sweepShadow) - delta : int(m_sweepShadow) + delta;
    if (next > 2047) {                 // overflow silences the channel
        m_sq[0].enabled = false;
        return;
    }
    if (m_sweepShift) {
        m_sweepShadow = uint16_t(next);
        m_sq[0].freq = m_sweepShadow;
    }
}

// 512 Hz, eight steps: length on the even ones, sweep on 2 and 6, envelope on 7.
void GbDmg::stepFrameSequencer()
{
    if ((m_seqStep & 1) == 0) {
        auto tickLength = [](int& len, bool enabled, bool& on) {
            if (enabled && len > 0 && --len == 0) on = false;
        };
        tickLength(m_sq[0].length, m_sq[0].lengthEnabled, m_sq[0].enabled);
        tickLength(m_sq[1].length, m_sq[1].lengthEnabled, m_sq[1].enabled);
        tickLength(m_wave.length, m_wave.lengthEnabled, m_wave.enabled);
        tickLength(m_noise.length, m_noise.lengthEnabled, m_noise.enabled);
    }
    if (m_seqStep == 2 || m_seqStep == 6) clockSweep();
    if (m_seqStep == 7) {
        clockEnvelope(m_sq[0].volume, m_sq[0].envTimer, m_sq[0].envPeriod, m_sq[0].envUp);
        clockEnvelope(m_sq[1].volume, m_sq[1].envTimer, m_sq[1].envPeriod, m_sq[1].envUp);
        clockEnvelope(m_noise.volume, m_noise.envTimer, m_noise.envPeriod, m_noise.envUp);
    }
    m_seqStep = (m_seqStep + 1) & 7;
}

void GbDmg::stepChip()
{
    if (++m_seqTimer >= 8192) { m_seqTimer = 0; stepFrameSequencer(); }

    for (int i = 0; i < 2; ++i) {
        if (--m_sq[i].timer <= 0) {
            m_sq[i].timer = (2048 - m_sq[i].freq) * 4;
            if (m_sq[i].timer <= 0) m_sq[i].timer = 1;
            m_sq[i].phase = (m_sq[i].phase + 1) & 7;
        }
    }

    if (--m_wave.timer <= 0) {
        m_wave.timer = (2048 - m_wave.freq) * 2;
        if (m_wave.timer <= 0) m_wave.timer = 1;
        m_wave.phase = (m_wave.phase + 1) & 31;
    }

    if (--m_noise.timer <= 0) {
        m_noise.timer = kNoiseDivisor[m_noise.divisor] << m_noise.shift;
        if (m_noise.timer <= 0) m_noise.timer = 1;
        const uint16_t bit = uint16_t((m_noise.lfsr ^ (m_noise.lfsr >> 1)) & 1);
        m_noise.lfsr = uint16_t((m_noise.lfsr >> 1) | (bit << 14));
        if (m_noise.narrow) {
            m_noise.lfsr = uint16_t((m_noise.lfsr & ~0x40) | (bit << 6));
        }
    }
}

void GbDmg::render(int32_t* left, int32_t* right, int frames)
{
    if (!left || !right || frames <= 0) return;

    static const int kWaveShift[4] = {4, 0, 1, 2};   // 0 is mute, then 1, 1/2, 1/4

    for (int i = 0; i < frames; ++i) {
        m_frac += m_step;
        while (m_frac >= 1.0) { stepChip(); m_frac -= 1.0; }

        int out[4] = {0, 0, 0, 0};
        if (m_powered) {
            for (int c = 0; c < 2; ++c) {
                if (m_sq[c].enabled && kDuty[m_sq[c].duty][m_sq[c].phase])
                    out[c] = m_sq[c].volume;
            }
            if (m_wave.enabled && m_wave.dacOn) {
                const uint8_t byte = m_wave.ram[m_wave.phase >> 1];
                const uint8_t nib = (m_wave.phase & 1) ? (byte & 0x0F) : (byte >> 4);
                out[2] = nib >> kWaveShift[m_wave.shift];
            }
            if (m_noise.enabled && !(m_noise.lfsr & 1))
                out[3] = m_noise.volume;
        }

        int l = 0, r = 0;
        for (int c = 0; c < 4; ++c) {
            if (m_nr51 & (0x10 << c)) l += out[c];
            if (m_nr51 & (0x01 << c)) r += out[c];
        }
        // Four channels of 0-15 through a 0-7 master volume. Scaled so a full
        // mix sits near the same level the other chips here are mixed at.
        const int volL = ((m_nr50 >> 4) & 7) + 1;
        const int volR = (m_nr50 & 7) + 1;
        left[i]  += (l * volL) * 24;
        right[i] += (r * volR) * 24;
    }
}

int GbDmg::level(int ch) const
{
    switch (ch) {
        case 0: return m_sq[0].enabled ? (m_sq[0].volume * 127 / 15) : 0;
        case 1: return m_sq[1].enabled ? (m_sq[1].volume * 127 / 15) : 0;
        case 2: return (m_wave.enabled && m_wave.shift) ? (127 >> (m_wave.shift - 1)) : 0;
        case 3: return m_noise.enabled ? (m_noise.volume * 127 / 15) : 0;
        default: return 0;
    }
}
