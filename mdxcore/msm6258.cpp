#include "msm6258.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

const int indexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

const int stepSizeTable[49] = {
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552
};

// What each value of the F command plays at. The five ADPCM rates are the
// MSM6258's 8 MHz clock over 2048, 1536, 1024, 768 and 512; the two PCM kinds
// run at the top one; 7 stops the channel (X68Sound ADPCMRATEADDTBL).
const double kFormatRate[8] = {
    3906.25, 5208.333, 7812.5, 10416.667, 15625.0, 15625.0, 15625.0, 0.0,
};

// X68Sound high-passes every PCM8 channel twice before summing them, at its
// internal 62.5 kHz: a pole of 1 - 1/32 - 1/1024 and one of
// 1 - 1/256 - 1/512 - 1/4096, which are 326 Hz and 61 Hz. The same corners
// re-placed at whatever rate we run at.
const double kHpfHz[2] = { 325.9, 60.9 };

const double kPi = 3.14159265358979323846;

} // namespace

Msm6258::Msm6258()
    : m_outputRate(44100)
    , m_sampleRate(15625.0)
    , m_format(4)
    , m_step(0)
    , m_frac(0)
    , m_pan(3)
    , m_volume(127)
    , m_playing(false)
    , m_loop(false)
    , m_data(nullptr)
    , m_dataSize(0)
    , m_dataPos(0)
    , m_nibbleFlag(false)
    , m_signal(0)
    , m_stepIndex(0)
    , m_pcmPrev(0)
    , m_lastSample(0)
    , m_prevSample(0)
    , m_hpA(0.0f), m_hpB(0.0f)
    , m_hp1In(0.0f), m_hp1Out(0.0f)
    , m_hp2In(0.0f), m_hp2Out(0.0f)
{
}

Msm6258::~Msm6258()
{
}

void Msm6258::init(int outputSampleRate)
{
    m_outputRate = (outputSampleRate > 0) ? outputSampleRate : 44100;
    m_hpA = float(std::exp(-2.0 * kPi * kHpfHz[0] / m_outputRate));
    m_hpB = float(std::exp(-2.0 * kPi * kHpfHz[1] / m_outputRate));
    reset();
}

void Msm6258::reset()
{
    m_playing = false;
    m_data = nullptr;
    m_dataSize = 0;
    m_dataPos = 0;
    m_nibbleFlag = false;
    m_signal = 0;
    m_stepIndex = 0;
    m_pcmPrev = 0;
    m_lastSample = 0;
    m_prevSample = 0;
    m_frac = 0;
    m_format = 4;
    m_sampleRate = kFormatRate[4];
    m_hp1In = m_hp1Out = m_hp2In = m_hp2Out = 0.0f;
    updateStep();
}

void Msm6258::updateStep()
{
    m_step = int(m_sampleRate * 65536.0 / m_outputRate + 0.5);
}

void Msm6258::setSampleRate(int rate)
{
    m_sampleRate = (rate > 0) ? rate : 15625.0;
    updateStep();
}

void Msm6258::setFormat(int format)
{
    m_format = format & 7;
    m_sampleRate = kFormatRate[m_format];
    updateStep();
    if (m_format == 7) stop();
}

void Msm6258::setPan(int pan)
{
    m_pan = pan;
}

// The volume byte a track carries, turned into the level the PCM actually
// plays at.
//
// This used to be `clamp(vol, 0, 127)`, used as a straight multiplier. Both
// halves of that were wrong. The driver keeps two tables (mxdrv200b L000e56
// and L000fba): a plain `v` 0-15 goes through the first, which turns it into
// an attenuation on a 0x2A..0x02 scale, then that attenuation - plus any
// fade-out offset - indexes the second, a 0x2B-entry table of the 0-15 level
// the PCM8 call actually receives. A value with bit 7 set skips the first
// table and is the attenuation itself.
void Msm6258::setVolume(int vol)
{
    // The driver's two tables, at L000e56 and L000fba.
    static const uint8_t kVolumeCurve[16] = {
        0x2A, 0x28, 0x25, 0x22, 0x20, 0x1D, 0x1A, 0x18,
        0x15, 0x12, 0x10, 0x0D, 0x0A, 0x08, 0x05, 0x02,
    };
    static const uint8_t kPcmLevel[0x2B] = {
        0x0F, 0x0F, 0x0F, 0x0E, 0x0E, 0x0E, 0x0D, 0x0D,
        0x0D, 0x0C, 0x0C, 0x0B, 0x0B, 0x0B, 0x0A, 0x0A,
        0x0A, 0x09, 0x09, 0x08, 0x08, 0x08, 0x07, 0x07,
        0x07, 0x06, 0x06, 0x05, 0x05, 0x05, 0x04, 0x04,
        0x04, 0x03, 0x03, 0x02, 0x02, 0x02, 0x01, 0x01,
        0x01, 0x00, 0x00,
    };

    const int byte = vol & 0xFF;
    const int atten = (byte & 0x80) ? (byte & 0x7F)
                                    : kVolumeCurve[byte & 0x0F];
    m_volume = (atten >= 0x2B) ? 0 : kPcmLevel[atten];
}

void Msm6258::start(const uint8_t* pcmData, size_t dataSize, bool loop)
{
    if (!pcmData || dataSize == 0 || m_format == 7) {
        stop();
        return;
    }
    m_data = pcmData;
    m_dataSize = dataSize;
    m_dataPos = 0;
    m_nibbleFlag = false;
    m_signal = 0;
    m_stepIndex = 0;
    m_pcmPrev = 0;
    m_lastSample = 0;
    m_prevSample = 0;
    m_frac = 0;
    // A key-on resets the decoder and both high-pass stages (Pcm8::Reset).
    m_hp1In = m_hp1Out = m_hp2In = m_hp2Out = 0.0f;
    m_loop = loop;
    m_playing = true;
}

void Msm6258::stop()
{
    m_playing = false;
    m_data = nullptr;
    m_dataSize = 0;
}

int Msm6258::decodeNibble(uint8_t nibble)
{
    int ss = stepSizeTable[m_stepIndex];
    int diff = ss >> 3;
    if (nibble & 1) diff += (ss >> 2);
    if (nibble & 2) diff += (ss >> 1);
    if (nibble & 4) diff += ss;

    if (nibble & 8) {
        m_signal -= diff;
    } else {
        m_signal += diff;
    }

    m_signal = std::clamp(m_signal, -2047, 2047);

    m_stepIndex += indexTable[nibble];
    m_stepIndex = std::clamp(m_stepIndex, 0, 48);

    // The DAC is ten bits wide, so the bottom two never reach it.
    return (m_signal & ~3) << 4; // Scale to 16-bit
}

// Decode one more sample into m_lastSample; false when the data ran out.
bool Msm6258::nextSample()
{
    if (m_format == 5) {                       // 16-bit PCM, big-endian
        if (m_dataPos + 2 > m_dataSize) return false;
        const int v = int(int16_t((m_data[m_dataPos] << 8) | m_data[m_dataPos + 1]));
        m_dataPos += 2;
        // X68Sound tracks the raw value inside the ADPCM's own 12-bit range
        // (Pcm8::pcm16_2pcm), so a PCM sample is clamped exactly as a decoded
        // one would be.
        m_signal = std::clamp(m_signal + (v - m_pcmPrev), -2047, 2047);
        m_pcmPrev = v;
        m_lastSample = (m_signal & ~3) << 4;
        return true;
    }
    if (m_format == 6) {                       // 8-bit PCM
        if (m_dataPos >= m_dataSize) return false;
        const int v = int(int8_t(m_data[m_dataPos++]));
        m_signal = std::clamp(m_signal + (v - m_pcmPrev), -2047, 2047);
        m_pcmPrev = v;
        m_lastSample = (m_signal & ~3) << 4;
        return true;
    }

    if (m_dataPos >= m_dataSize) return false;
    // The X68000 plays the LOW nibble of each byte first, then the high one
    // (X68Sound Pcm8::GetPcm, "N10Data & 0x0F" before "N10Data >> 4"). This
    // read them the other way round, and ADPCM decoded in the wrong order is
    // not merely noisy: the step size runs away and the signal pins itself
    // at the clamp, so every drum came out as a saturated thud with about the
    // right loudness and none of the transient. Proven with a 5 kHz tone
    // packed both ways and rendered by mxwav: only low-first plays a tone.
    const uint8_t byte = m_data[m_dataPos];
    uint8_t nibble;
    if (!m_nibbleFlag) {
        nibble = byte & 0x0F;
        m_nibbleFlag = true;
    } else {
        nibble = (byte >> 4) & 0x0F;
        m_nibbleFlag = false;
        m_dataPos++;
    }
    m_lastSample = decodeNibble(nibble);
    return true;
}

void Msm6258::render(int32_t* left, int32_t* right, int sampleCount)
{
    if (!m_playing || !m_data || m_dataSize == 0) return;

    for (int i = 0; i < sampleCount; ++i) {
        while (m_frac >= 0x10000) {
            m_frac -= 0x10000;
            m_prevSample = m_lastSample;
            if (!nextSample()) {
                if (m_loop && m_dataSize > 0) {
                    m_dataPos = 0;
                    m_nibbleFlag = false;
                    if (!nextSample()) { m_playing = false; break; }
                } else {
                    m_playing = false;
                    break;
                }
            }
        }

        if (!m_playing) break;

        m_frac += m_step;

        // Straight between the two decoded samples rather than holding one.
        //
        // The ADPCM runs at 15625 Hz and the device at 44100, so a hold repeats
        // each sample nearly three times - which is a rectangular pulse, and a
        // rectangular pulse puts images either side of 15.6 kHz. Measured
        // against mxwav on DK_03 that was +10 dB above 10 kHz with the cymbal's
        // own 2.5-10 kHz missing: a crash arriving as a hiss.
        const int32_t interp = m_prevSample
            + int32_t(((int64_t)(m_lastSample - m_prevSample) * m_frac) >> 16);

        // The two high-pass stages, in series, on this channel alone.
        const float x = float(interp);
        const float h1 = m_hpA * m_hp1Out + x - m_hp1In;
        m_hp1In = x; m_hp1Out = h1;
        const float h2 = m_hpB * m_hp2Out + h1 - m_hp2In;
        m_hp2In = h1; m_hp2Out = h2;

        // m_volume is the 0-15 level setVolume() resolved. PCM8 does not use
        // it as a straight multiplier - it looks it up (X68Sound global.h,
        // PCM8VOLTBL) and the curve is close to exponential, a 40:1 span where
        // a linear 0-15 gives 15:1. Using the level directly flattened the
        // PCM's dynamics: loud hits not loud enough, quiet ones not quiet.
        // Level 8 - the volume a track that never sets one starts at - is 16
        // here, and the overall PCM gain in Mxdrv is fitted with that in mind.
        static const int kPcm8Vol[16] = {
            2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80,
        };
        const int32_t sample = int32_t(h2) * kPcm8Vol[m_volume & 15] / 16;

        // PCM8 pans with the OPM's own bit order: bit 0 is left, bit 1 right
        // (X68Sound opm.h, "OutInpAdpcm[0] += (-(pan&1)) & o"). It was the
        // other way round here, which mirrored p1/p2 on PCM tracks.
        if (m_pan & 1) left[i] += sample;
        if (m_pan & 2) right[i] += sample;
    }
}

void X68PcmBus::init(int outputSampleRate)
{
    const double fs = double(outputSampleRate > 0 ? outputSampleRate : 44100);
    {
        // z^2 - (1537/1024) z + 617/1024 at 62.5 kHz: poles at r = 0.776,
        // 0.258 rad, which is f0 = 3598 Hz, Q = 0.714.
        const double f0 = 3598.0, q = 0.714;
        const double w0 = 2.0 * kPi * f0 / fs;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double c = std::cos(w0);
        const double a0 = 1.0 + alpha;
        m_b0 = float((1.0 - c) / 2.0 / a0);
        m_b1 = float((1.0 - c) / a0);
        m_b2 = m_b0;
        m_a1 = float(-2.0 * c / a0);
        m_a2 = float((1.0 - alpha) / a0);
    }
    {
        // (x + x1) * 356/1024 + y1 * 312/1024 at 62.5 kHz: a pole of 0.305,
        // which is 11.8 kHz.
        const double fc = 11822.0;
        const double k = std::tan(kPi * fc / fs);
        m_c0 = float(k / (1.0 + k));
        m_d1 = float((1.0 - k) / (1.0 + k));
    }
    reset();
}

void X68PcmBus::reset()
{
    m_st[0] = State();
    m_st[1] = State();
}

void X68PcmBus::mixInto(const int32_t* pcmL, const int32_t* pcmR,
                        int32_t* outL, int32_t* outR, int n) const
{
    const int32_t* in[2] = { pcmL, pcmR };
    int32_t* out[2] = { outL, outR };
    for (int c = 0; c < 2; ++c) {
        State& f = m_st[c];
        for (int i = 0; i < n; ++i) {
            const float x = float(in[c][i]);
            const float y = m_b0 * x + m_b1 * f.x1 + m_b2 * f.x2 - m_a1 * f.y1 - m_a2 * f.y2;
            f.x2 = f.x1; f.x1 = x; f.y2 = f.y1; f.y1 = y;
            const float z = m_c0 * (y + f.u1) + m_d1 * f.v1;
            f.u1 = y; f.v1 = z;
            out[c][i] += int32_t(z * kGain);
        }
    }
}
