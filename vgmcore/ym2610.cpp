#include "ym2610.h"

#include "ymfm/ymfm_opn.h"

#include <algorithm>
#include <cstring>

namespace {

// The chip's sample rate at ymfm's lowest fidelity is clock/144 - 55.5 kHz on
// the Neo Geo's 8 MHz part. The device runs at 49716, so this is a small
// downward ratio and the frames a single output sample spans are averaged
// rather than dropped. A box filter over one or two samples is crude, but
// dropping every ninth sample instead would fold the top octave back down.
const uint32_t kDivider = 144;

// A level fit, like SegaPCM's halving and QSound's 4.2 - ymfm's own output is
// not scaled for a host mixer either way. Measured as loudest-5s RMS, the two
// Neo Geo files sat at 0.107 and 0.055 against the OPL engines' 0.108-0.183,
// so the chip played about 5 dB under everything else in the player. 1.7 puts
// them at 0.18 and 0.09 with peaks of 0.88 and 0.45, and the OPL engines
// themselves reach 0.96.
const double kMixGain = 1.7;

} // namespace

// The chip reads its two sample ROMs back by address while it plays, so the
// interface has to outlive every generate() call.
struct Ym2610::Impl : public ymfm::ymfm_interface
{
    Impl() : chip(*this) {}

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
    {
        const std::vector<uint8_t>* rom =
            (type == ymfm::ACCESS_ADPCM_A) ? &romA :
            (type == ymfm::ACCESS_ADPCM_B) ? &romB : nullptr;
        if (!rom || address >= rom->size()) return 0;
        return (*rom)[address];
    }

    ymfm::ym2610 chip;
    std::vector<uint8_t> romA, romB;

    int      clock = 8000000;
    int      sampleRate = 44100;
    double   step = 1.0;      // chip samples per output frame
    double   frac = 0.0;
    // The most recent register write per channel, which is all the monitor can
    // have: nothing in the chip reports a level.
    uint8_t  fmKey[4] = { 0 };
    uint8_t  ssgVol[3] = { 0 };
    uint8_t  admA[6] = { 0 };
    uint8_t  admB = 0;
};

Ym2610::Ym2610() : m_impl(new Impl()) {}
Ym2610::~Ym2610() = default;

void Ym2610::init(int clock, int sampleRate)
{
    Impl& d = *m_impl;
    d.clock = clock > 0 ? clock : 8000000;
    d.sampleRate = sampleRate > 0 ? sampleRate : 44100;
    d.chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);
    d.step = double(d.clock) / double(kDivider) / double(d.sampleRate);
    reset();
}

void Ym2610::reset()
{
    Impl& d = *m_impl;
    d.chip.reset();
    d.frac = 0.0;
    std::memset(d.fmKey, 0, sizeof(d.fmKey));
    std::memset(d.ssgVol, 0, sizeof(d.ssgVol));
    std::memset(d.admA, 0, sizeof(d.admA));
    d.admB = 0;
}

static void placeRom(std::vector<uint8_t>& rom, const uint8_t* data, size_t size,
                     size_t offset, size_t totalSize)
{
    if (!data || size == 0) return;
    const size_t want = std::max(totalSize, offset + size);
    if (rom.size() < want) rom.resize(want, 0);
    std::memcpy(rom.data() + offset, data, size);
}

void Ym2610::setAdpcmRomA(const uint8_t* data, size_t size, size_t offset, size_t totalSize)
{
    placeRom(m_impl->romA, data, size, offset, totalSize);
}

void Ym2610::setAdpcmRomB(const uint8_t* data, size_t size, size_t offset, size_t totalSize)
{
    placeRom(m_impl->romB, data, size, offset, totalSize);
}

void Ym2610::write(int port, uint8_t reg, uint8_t value)
{
    Impl& d = *m_impl;
    if (port == 0) { d.chip.write_address(reg);    d.chip.write_data(value); }
    else           { d.chip.write_address_hi(reg); d.chip.write_data_hi(value); }

    // Monitor state only - none of this affects the chip.
    if (port == 0) {
        // 0x28 keys the FM channels; the low two bits pick one of the four.
        if (reg == 0x28) {
            const int ch = value & 0x03;
            if (ch < 4) d.fmKey[ch] = (value & 0xF0) ? 110 : 0;
        }
        // 0x08-0x0A are the SSG channel amplitudes.
        else if (reg >= 0x08 && reg <= 0x0A) d.ssgVol[reg - 0x08] = value & 0x1F;
        // 0x08-0x0D of the ADPCM-A bank are its per-channel levels, but those
        // live on port 1; port 0's 0x100 block is the ADPCM-B.
        else if (reg == 0x1B) d.admB = value;
    } else {
        if (reg >= 0x08 && reg <= 0x0D) d.admA[reg - 0x08] = value & 0x1F;
    }
}

void Ym2610::render(int32_t* left, int32_t* right, int frames)
{
    if (!left || !right || frames <= 0) return;
    Impl& d = *m_impl;

    for (int i = 0; i < frames; ++i) {
        int64_t accL = 0, accR = 0;
        int n = 0;
        d.frac += d.step;
        while (d.frac >= 1.0) {
            ymfm::ym2610::output_data out;
            d.chip.generate(&out, 1);
            // FM is stereo on the first two outputs; the SSG is the mono third
            // and goes to both, which is how the hardware sums them.
            accL += int64_t(out.data[0]) + out.data[2];
            accR += int64_t(out.data[1]) + out.data[2];
            ++n;
            d.frac -= 1.0;
        }
        if (n > 0) {
            left[i]  += int32_t(accL * kMixGain / n);
            right[i] += int32_t(accR * kMixGain / n);
        }
    }
}

int Ym2610::level(int ch) const
{
    const Impl& d = *m_impl;
    if (ch < 0) return 0;
    if (ch < 4)  return d.fmKey[ch];
    if (ch < 7)  return int(d.ssgVol[ch - 4]) * 127 / 31;
    if (ch < 13) return int(d.admA[ch - 7]) * 127 / 31;
    if (ch == 13) return (d.admB & 0x80) ? 110 : 0;
    return 0;
}
