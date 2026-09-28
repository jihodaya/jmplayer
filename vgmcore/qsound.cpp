#include "qsound.h"

extern "C" {
#include "qsoundhle/qsound.h"
}

#include <algorithm>
#include <cstring>

namespace {

// A fitted mixing choice, not a correction - said plainly because the chip's
// own output stage (`(wet + dry) << 2 >> 16`) is its fixed-point arithmetic,
// not headroom left for a host to reclaim, so unlike emu2413's halved melody
// voices there is nothing here to "undo".
//
// Fitted against the PLAYER, not against other VGMs: the OPL engines are what
// a listener has in their ear, and they sit at 0.108-0.183 loudest-5s RMS
// (mean 0.138). Matching the VGM corpus instead left this 5 dB under, which is
// what "still quieter than everything else" was. See jmp/CLAUDE.md 7-9.
//
// The ceiling is the peak: 4.2 puts the two files here at 0.93 and 0.63, and
// the OPL engines themselves reach 0.96, so this is as far as it goes without
// clipping.
const double kMixGain = 4.2;

} // namespace

struct Qsound::Impl
{
    qsound_chip chip {};
    std::vector<uint8_t> rom;

    int    sampleRate = 44100;
    double chipRate = 24038.0;
    double step = 1.0;        // chip samples per output frame
    double frac = 0.0;
    // The chip runs at about half the device rate, so each of its samples
    // spans two output frames; the pair either side is interpolated rather
    // than held, which a 2:1 stretch would otherwise make audibly hard.
    int16_t prevL = 0, prevR = 0, curL = 0, curR = 0;
    bool    primed = false;

    // Monitor state. The chip has no readable level, so this follows the
    // volume register each channel is given.
    uint16_t vol[16] = { 0 };

    void advance()
    {
        prevL = curL; prevR = curR;
        int16_t l = 0, r = 0;
        int16_t* outs[2] = { &l, &r };
        qsound_stream_update(&chip, outs, 1);
        curL = l; curR = r;
    }
};

Qsound::Qsound() : m_impl(new Impl()) {}
Qsound::~Qsound() = default;

void Qsound::init(int clock, int sampleRate)
{
    Impl& d = *m_impl;
    d.sampleRate = sampleRate > 0 ? sampleRate : 44100;

    // A VGM declares 4000000 here, which is the board's QSound reference and
    // one fifteenth of what the DSP16A actually runs at. qsound_start() wants
    // the DSP clock - it returns clock / 2 / 1248, the program's machine cycles
    // per iteration - so handing it the header value straight ran the chip at
    // 1602 Hz instead of 24038 and the music was unrecognisable.
    //
    // A file that already names the DSP clock is taken at its word.
    int dspClock = clock > 0 ? clock : 4000000;
    if (dspClock < 30000000) dspClock *= 15;

    const long rate = qsound_start(&d.chip, dspClock);
    d.chipRate = rate > 0 ? double(rate) : 24038.0;
    d.step = d.chipRate / double(d.sampleRate);
    // qsound_start memsets the chip, so the ROM pointer has to go back after.
    if (!d.rom.empty()) {
        d.chip.rom_data = d.rom.data();
        d.chip.rom_mask = static_cast<unsigned long>(d.rom.size() - 1);
    }
    reset();
}

void Qsound::reset()
{
    Impl& d = *m_impl;
    qsound_reset(&d.chip);
    d.frac = 0.0;
    d.prevL = d.prevR = d.curL = d.curR = 0;
    d.primed = false;
    std::memset(d.vol, 0, sizeof(d.vol));
}

void Qsound::setRom(const uint8_t* data, size_t size, size_t offset, size_t totalSize)
{
    if (!data || size == 0) return;
    Impl& d = *m_impl;
    // Rounded up to a power of two, because the chip addresses it through a
    // mask rather than a length.
    size_t want = std::max(totalSize, offset + size);
    size_t pow2 = 1;
    while (pow2 < want) pow2 <<= 1;
    if (d.rom.size() < pow2) d.rom.resize(pow2, 0);
    std::memcpy(d.rom.data() + offset, data, size);
    d.chip.rom_data = d.rom.data();
    d.chip.rom_mask = static_cast<unsigned long>(d.rom.size() - 1);
}

void Qsound::write(uint8_t reg, uint16_t value)
{
    Impl& d = *m_impl;
    qsound_write_data(&d.chip, reg, value);
    // Registers 0x00-0x7F are eight words per voice; word 6 is the volume.
    if (reg < 0x80 && (reg & 0x07) == 0x06) d.vol[reg >> 3] = value;
}

void Qsound::render(int32_t* left, int32_t* right, int frames)
{
    if (!left || !right || frames <= 0) return;
    Impl& d = *m_impl;
    if (!d.primed) { d.advance(); d.advance(); d.primed = true; }

    for (int i = 0; i < frames; ++i) {
        d.frac += d.step;
        while (d.frac >= 1.0) { d.advance(); d.frac -= 1.0; }
        const double t = d.frac;
        left[i]  += int32_t((d.prevL + (d.curL - d.prevL) * t) * kMixGain);
        right[i] += int32_t((d.prevR + (d.curR - d.prevR) * t) * kMixGain);
    }
}

int Qsound::level(int ch) const
{
    if (ch < 0 || ch >= kVoices) return 0;
    const int v = std::abs(int(int16_t(m_impl->vol[ch])));
    // The register is 16 bits wide but the music does not use that range: the
    // loudest write in either file here is 0x0A8E, and 0x065A in the quieter
    // one. Scaled against the register's width instead, a struck note read 42
    // where every other chip's bar reaches 110, so the monitor looked dead.
    return std::min(127, v * 127 / 0x0C00);
}
