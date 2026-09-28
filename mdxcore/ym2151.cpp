#include <cstdio>
#include "ym2151.h"

extern "C" {
#include "opm.h"
}

#include <cmath>
#include <cstring>

#define CHIP (static_cast<opm_t*>(m_chip))
#include <cstdlib>

namespace {
// Nuked-OPM advances one master-clock cycle per OPM_Clock() and its internal
// slot counter wraps every 32. The DAC latches a finished stereo pair once per
// wrap, so a chip sample is 32 calls - which at the X68000's 4 MHz gives
// 125 kHz, twice the 62.5 kHz the chip actually outputs, because the real part
// divides the clock by two on the way in. Hence 64.
// Two numbers, and conflating them ran the chip at double speed.
//
// OPM_Clock advances one INTERNAL cycle - `chip->cycles = (chip->cycles + 1) &
// 31` at the end of it - so 32 calls complete one sample. But the real chip
// finishes a sample every 64 MASTER clocks, because the master clock is halved
// before the slot counter. One call is therefore two master clocks.
//
// The old code used a single "64" for both: it called OPM_Clock 64 times per
// sample and then declared those samples to arrive at clock/64. Either way
// round the chip advanced `clock` cycles for every second of output where it
// should advance clock/2, so its whole sense of time ran double - every note an
// octave sharp and every envelope twice as fast.
//
// Measured on a one-note MDX built for the purpose, same voice, same note:
// mxwav puts the fundamental at 110 Hz and this put it at 220. Chroma never saw
// it, because a pitch-class profile is octave-blind - which is how 101 songs
// scored 0.915 with this in place.
const int kCyclesPerChipSample = 32;    // calls to OPM_Clock for one sample
const int kMasterClocksPerSample = 64;  // what that sample costs in master clocks
}

Ym2151::Ym2151()
    : m_chip(nullptr)
    , m_clock(4000000)
    , m_rate(44100)
    , m_step(1.0)
    , m_pos(0.0)
{
    std::memset(m_hist, 0, sizeof(m_hist));
    m_histPos = 0;
    m_pendHead = m_pendTail = 0;
    m_writeStage = 0;
    m_writeWait = 0;
    m_directWrites = false;
    m_chip = std::calloc(1, sizeof(opm_t));
}

Ym2151::~Ym2151()
{
    std::free(m_chip);
    m_chip = nullptr;
}

void Ym2151::init(int clock, int rate)
{
    m_clock = (clock > 0) ? clock : 4000000;
    m_rate = (rate > 0) ? rate : 44100;
    buildFilter();

    // No colouring here. The rolloff that matches mxwav is jmp's own DSP
    // switch, applied at the player - see the note in mdxplayer.cpp.
    m_tiltA = 0.0f;
    // The one thing that is not colouring: the chip's output is AC-coupled on
    // the board, and X68Sound blocks DC with a pole of 1 - 1/1024 - 1/4096 at
    // its 62.5 kHz, which is 12 Hz. Heavy feedback voices put out a large DC
    // term - SF2_BON's pads measured 4.4 dB more RMS than mxwav with the
    // audible partials matching to 0.2 dB, all of it below 20 Hz.
    m_dcA = float(std::exp(-2.0 * 3.14159265358979 * 12.1 / double(m_rate)));
    reset();
}

// A windowed-sinc fractional-delay lowpass, one set of taps per phase.
//
// The cutoff has to sit below the OUTPUT rate's Nyquist, not the chip's -
// everything above it is what folds down when the stream is decimated. Nine
// tenths of it leaves a transition band the 32 taps can actually make.
void Ym2151::buildFilter()
{
    const double inRate = double(m_clock) / double(kMasterClocksPerSample);
    // Fraction of the INPUT Nyquist to keep.
    double c = 0.9 * (double(m_rate) / inRate);
    if (c > 0.95) c = 0.95;          // upsampling: keep nearly everything
    if (c < 0.05) c = 0.05;

    for (int p = 0; p < kPhases; ++p) {
        const double d = double(p) / double(kPhases);
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            // The output sits d input-samples AFTER the newest one in the
            // history, so a larger d moves the filter's peak towards the more
            // recent taps - minus, not plus. With the sign the other way the
            // fractional delay runs backwards over every phase, which is a
            // time-varying error of up to one sample: it measured as MORE
            // top-octave energy than plain linear interpolation, not less.
            const double x = (double(kTaps - 1) / 2.0) - d - double(k);
            const double a = 3.14159265358979 * c * x;
            const double sinc = (fabs(a) < 1e-9) ? 1.0 : (sin(a) / a);
            // Blackman window over the tap positions.
            const double t = double(k) / double(kTaps - 1);
            const double w = 0.42 - 0.5 * cos(2.0 * 3.14159265358979 * t)
                                  + 0.08 * cos(4.0 * 3.14159265358979 * t);
            const double h = c * sinc * w;
            m_fir[p][k] = float(h);
            sum += h;
        }
        // Unity gain at DC, so the filter cannot change the overall level.
        if (sum > 1e-9) {
            for (int k = 0; k < kTaps; ++k) m_fir[p][k] = float(m_fir[p][k] / sum);
        }
    }
}

void Ym2151::reset()
{
    if (!m_chip) return;
    OPM_Reset(CHIP, opm_flags_none);

    const double chipRate = double(m_clock) / double(kMasterClocksPerSample);
    m_step = chipRate / double(m_rate);
    m_pos = 0.0;
    std::memset(m_hist, 0, sizeof(m_hist));
    m_histPos = 0;
    m_tiltZ[0] = m_tiltZ[1] = 0.0f;
    m_dcIn[0] = m_dcIn[1] = m_dcOut[0] = m_dcOut[1] = 0.0f;
    m_pendHead = m_pendTail = 0;
    m_writeStage = 0;
    m_writeWait = 0;

    // Prime the filter's history so the first output frames are not a ramp up
    // from silence through thirty-two taps.
    for (int i = 0; i < kTaps; ++i) {
        int32_t s[2];
        clockOne(s);
        m_histPos = (m_histPos + 1) % kTaps;
        m_hist[0][m_histPos] = s[0];
        m_hist[1][m_histPos] = s[1];
    }
}

void Ym2151::clockOne(int32_t* out)
{
    int32_t o[2] = { 0, 0 };
    for (int i = 0; i < kCyclesPerChipSample; ++i) {
        pumpWrites();
        OPM_Clock(CHIP, o, nullptr, nullptr, nullptr);
    }
    out[0] = o[0];
    out[1] = o[1];
}

// OPM_Write only flags a pending write, and the address and the data share one
// holding register - so the chip has to be clocked between the two, or the data
// overwrites the address and neither lands.
//
// That used to be done here, by clocking the chip 32 cycles after each half and
// THROWING THE AUDIO AWAY. Two of the chip's own samples went missing per
// register write. The driver writes constantly - a volume per note, a key code
// per tick once the software LFO runs - so the chip was being fast-forwarded
// past hundreds of samples a second, which smears every envelope and shifts the
// pitch of the resampler that reads it. Adding the LFO's writes made it
// unmistakable: with the modulation depth divided by thirty-two, so that it
// wrote the SAME key code it had just written, a song still fell from 0.990 to
// 0.582 against the reference render.
//
// So writes are queued instead, and handed over while the chip is being clocked
// for audio. The cost is a delay of up to 64 cycles - a millisecond at worst,
// and the order is kept - rather than a hole in the output.
void Ym2151::writeReg(uint8_t reg, uint8_t data)
{
    if (!m_chip) return;
    if (m_directWrites) { writeRegNow(reg, data); return; }
    const int next = (m_pendTail + 1) % kPendSize;
    if (next == m_pendHead) return;         // 4096 deep; nothing gets near it
    m_pend[m_pendTail].reg = reg;
    m_pend[m_pendTail].data = data;
    m_pendTail = next;

    if (getenv("MDX_QDEPTH")) {
        const int depth = (m_pendTail - m_pendHead + kPendSize) % kPendSize;
        static int worst = 0;
        static long total = 0;
        ++total;
        if (depth > worst) {
            worst = depth;
            fprintf(stderr, "queue depth %d (%ld writes so far, %d cycles behind)\n",
                    worst, total, worst * 64);
        }
    }
}

// The immediate form, for when nothing is listening: clock the chip between the
// two halves and throw those cycles away, since no audio is being taken from it.
void Ym2151::writeRegNow(uint8_t reg, uint8_t data)
{
    int32_t o[2];
    OPM_Write(CHIP, 0, reg);
    for (int i = 0; i < 32; ++i) OPM_Clock(CHIP, o, nullptr, nullptr, nullptr);
    OPM_Write(CHIP, 1, data);
    for (int i = 0; i < 32; ++i) OPM_Clock(CHIP, o, nullptr, nullptr, nullptr);
}

// One chip cycle's worth of the handover, called from the audio clock loop.
void Ym2151::pumpWrites()
{
    if (m_writeWait > 0) { --m_writeWait; return; }

    if (m_writeStage == 1) {
        OPM_Write(CHIP, 1, m_pend[m_pendHead].data);
        m_pendHead = (m_pendHead + 1) % kPendSize;
        m_writeStage = 0;
        m_writeWait = 32;
        return;
    }
    if (m_pendHead != m_pendTail) {
        OPM_Write(CHIP, 0, m_pend[m_pendHead].reg);
        m_writeStage = 1;
        m_writeWait = 32;
    }
}

uint8_t Ym2151::readStatus()
{
    return m_chip ? OPM_Read(CHIP, 0) : 0;
}

void Ym2151::render(int32_t* left, int32_t* right, int sampleCount)
{
    if (!m_chip) return;

    for (int i = 0; i < sampleCount; ++i) {
        while (m_pos >= 1.0) {
            int32_t s[2];
            clockOne(s);
            if (m_tiltA > 0.0f) {
                m_tiltZ[0] += m_tiltA * (float(s[0]) - m_tiltZ[0]);
                m_tiltZ[1] += m_tiltA * (float(s[1]) - m_tiltZ[1]);
                s[0] = int32_t(m_tiltZ[0]);
                s[1] = int32_t(m_tiltZ[1]);
            }
            m_histPos = (m_histPos + 1) % kTaps;
            m_hist[0][m_histPos] = s[0];
            m_hist[1][m_histPos] = s[1];
            m_pos -= 1.0;
        }

        // Raw DAC value. Nuked-OPM already mixes the eight channels, so this
        // is the whole FM output and its full swing is about +-32768; the
        // caller scales it along with the ADPCM. There used to be a divide by
        // four here to make it fit a master scale that had been calibrated for
        // a different emulator - two wrongs that only cancelled by accident.
        int phase = int(m_pos * double(kPhases));
        if (phase < 0) phase = 0;
        if (phase >= kPhases) phase = kPhases - 1;
        const float* h = m_fir[phase];

        float accL = 0.0f, accR = 0.0f;
        int idx = m_histPos;
        for (int k = 0; k < kTaps; ++k) {
            accL += h[k] * float(m_hist[0][idx]);
            accR += h[k] * float(m_hist[1][idx]);
            if (--idx < 0) idx = kTaps - 1;
        }

        // The DC blocker, one pole at 12 Hz - see init().
        const float dl = m_dcA * m_dcOut[0] + accL - m_dcIn[0];
        m_dcIn[0] = accL; m_dcOut[0] = dl;
        const float dr = m_dcA * m_dcOut[1] + accR - m_dcIn[1];
        m_dcIn[1] = accR; m_dcOut[1] = dr;

        left[i]  += int32_t(dl);
        right[i] += int32_t(dr);
        m_pos += m_step;
    }
}

// The driver polls these to pace itself. Nuked-OPM keeps the real status bits,
// so the timers come from the chip rather than from a count of samples.
bool Ym2151::isTimerAOverflow() const
{
    return m_chip && (OPM_Read(static_cast<opm_t*>(const_cast<void*>(m_chip)), 0) & 0x01) != 0;
}

bool Ym2151::isTimerBOverflow() const
{
    return m_chip && (OPM_Read(static_cast<opm_t*>(const_cast<void*>(m_chip)), 0) & 0x02) != 0;
}

// Through the queue like everything else: writing the chip directly from here
// would overwrite the address half of whatever write is mid-handover.
void Ym2151::resetTimerAOverflow()
{
    writeReg(0x14, 0x11);
}

void Ym2151::resetTimerBOverflow()
{
    writeReg(0x14, 0x22);
}

void Ym2151::advanceTimers(int)
{
    // Nothing to do: the timers run inside the chip and advance with it.
}
