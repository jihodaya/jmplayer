#ifndef VGM_QSOUND_H
#define VGM_QSOUND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief Capcom DL-1425 "QSound" - 16 PCM channels with positional filtering.
 *
 * A DSP16A running a mask ROM rather than a synthesis model, which is why this
 * is not written out here: the sound is the program, and the program is what
 * places each channel in space with a FIR pair and an echo line.
 *
 * Wraps qsound-hle (Ian Karlsson and ValleyBell, BSD-3-Clause) - a from-scratch
 * high-level emulator written against a disassembly of that ROM. See
 * vgmcore/qsoundhle/LICENSE.
 */
class Qsound
{
public:
    Qsound();
    ~Qsound();
    Qsound(const Qsound&) = delete;
    Qsound& operator=(const Qsound&) = delete;

    void init(int clock, int sampleRate);
    void reset();

    // The sample ROM, from VGM data block type 0x8F.
    void setRom(const uint8_t* data, size_t size, size_t offset, size_t totalSize);

    // VGM command 0xC4: a 16-bit value into one of 256 registers.
    void write(uint8_t reg, uint16_t value);

    // Adds into the buffer, so several chips can share one mix.
    void render(int32_t* left, int32_t* right, int frames);

    static const int kVoices = 16;
    int level(int ch) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // VGM_QSOUND_H
