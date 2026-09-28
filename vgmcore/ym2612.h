#ifndef VGM_YM2612_H
#define VGM_YM2612_H

#include <cstdint>
#include <deque>

/**
 * @brief Yamaha YM2612 (Mega Drive), on Nuked-OPN2.
 *
 * Shaped like the Ym2151 wrapper next door so vgmplayer.cpp does not have to
 * know which emulator is underneath either chip.
 */
class Ym2612
{
public:
    Ym2612();
    ~Ym2612();

    // clock is the master clock (7670448 Hz on a Mega Drive). The chip's own
    // output rate is clock/144 and is resampled to rate.
    void init(int clock, int rate);
    void reset();

    // port is 0..3: address low, data low, address high, data high.
    void write(uint8_t port, uint8_t data);

    void render(int32_t* left, int32_t* right, int frames);
    // Clocks only far enough to hand the chip everything queued for it, for
    // seeking - the registers end up right, no samples are produced.
    void drain();

private:
    void clockNative(int32_t* out);

    // opaque: ym3438.h typedefs an anonymous struct, so it cannot be forward
    // declared - the same reason the Ym2151 wrapper keeps a void*.
    void* m_chip = nullptr;
    int m_clock = 7670448;
    int m_rate = 44100;

    // The chip only takes a write when it is not busy, and a burst of writes
    // arrives faster than that. Queueing them and letting the chip take one
    // whenever it is free is what the hardware made the 68000 do by polling.
    std::deque<uint16_t> m_queue;      // (port << 8) | data

    double m_step = 1.0;
    double m_pos = 0.0;
    int32_t m_prev[2] = {0, 0};
    int32_t m_next[2] = {0, 0};
};

#endif // VGM_YM2612_H
