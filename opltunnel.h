//
// opltunnel.h
//
// jmp (JJoMe player) side of the "OPL register tunnel" - MUST stay in sync with
// the jukebox's src/opl/midi/opltunnel.h. Streams jmp's Nuked-OPL3 register
// writes to the bare-metal jukebox over MIDI SysEx (via the Pico GPIO bridge),
// so an OPL song plays on the Pi's real DAC without rebooting the Pi.
//
//   F0 7D 4A <cmd> [payload...] F7
//     0x7D = reserved non-commercial manufacturer ID (safely ignored by mt32-pi
//            and other synths)
//     0x4A = sub-ID 'J' (jukebox OPL tunnel)
//     cmd 0x01 RESET : reset + OPL3-enable the chip (sent at OPL song start)
//     cmd 0x02 WRITE : payload = N x 3 bytes, each a 7-bit-packed (reg9,val8)
//
#ifndef JMP_OPLTUNNEL_H
#define JMP_OPLTUNNEL_H

#include <cstdint>

namespace OplTunnel {

constexpr uint8_t ManufacturerNonCommercial = 0x7D;
constexpr uint8_t SubID                     = 0x4A; // 'J'

enum Command : uint8_t {
    CmdReset = 0x01,
    CmdWrite = 0x02, // apply immediately (enable-time snapshot)
    CmdBatch = 0x03, // dtLo7 dtHi7 (ms since previous batch) + N x 3-byte writes
    // Virtual-stereo mode (1-9, jmp's pan patterns): payload = 1 byte. Sent
    // whenever jmp's stereo mode changes and after every resync snapshot, so
    // the receiver's OPL pan policy follows jmp's setting (2026-07-16).
    CmdStereo = 0x04,
    // Master volume, 0-100 percent in one byte. The tunnel occupies the same
    // 31250 baud wire as ordinary MIDI, so jmp's volume slider cannot simply
    // send sixteen CC#7 messages - that costs ~15 ms of wire time and pushes
    // the receiver's batch schedule past its cushion, which is audible. The
    // receiver also ignores channel messages while tunnelling, so they would
    // achieve nothing anyway. One byte, sent only when the slider settles,
    // costs nothing next to a batch (2026-08-06).
    CmdVolume = 0x05,
};

constexpr unsigned BytesPerWrite = 3;

// Pack a (reg, val) write into three 7-bit bytes. reg is the full 9-bit OPL3
// register (bank in bit 8); val is 8-bit. Mirror of the jukebox's Pack().
inline void Pack(uint16_t reg, uint8_t val, uint8_t out[3]) {
    const uint32_t word = ((uint32_t)(reg & 0x1FF) << 8) | val;
    out[0] = (uint8_t)(word & 0x7F);
    out[1] = (uint8_t)((word >> 7) & 0x7F);
    out[2] = (uint8_t)((word >> 14) & 0x7F);
}

} // namespace OplTunnel

#endif
