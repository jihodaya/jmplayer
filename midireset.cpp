//
// midireset.cpp
//
#include "midireset.h"

MidiReset::MidiReset()
    : m_enabled(false),
      m_flags(DefaultGMGS),
      m_delayMs(DefaultDelayMs) {
}

void MidiReset::SetSendCallback(std::function<void(const std::vector<uint8_t>&)> cb) {
    m_send = std::move(cb);
}

std::vector<uint8_t> MidiReset::BuildMessage(Flag which) {
    switch (which) {
    case GM:
        // GM System On (GM Level 1): F0 7E 7F 09 01 F7
        return {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    case GS:
        // Roland GS Reset: F0 41 10 42 12 40 00 7F 00 41 F7
        // (address 40 00 7F, data 00; 41 = checksum over 40 00 7F 00)
        return {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    case XG:
        // Yamaha XG System On: F0 43 10 4C 00 00 7E 00 F7
        return {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
    case MT32:
        // MT-32 master reset (Roland, model 16): F0 41 10 16 12 7F 00 01 00 F7
        return {0xF0, 0x41, 0x10, 0x16, 0x12, 0x7F, 0x00, 0x01, 0x00, 0xF7};
    default:
        return {};
    }
}

void MidiReset::SendResets() const {
    if (!m_enabled || m_flags == None || !m_send)
        return;

    // Fixed, device-mode-safe order (see header): GM clears to the plain
    // baseline first, then GS / XG raise the device into their superset mode -
    // whichever is sent LAST wins the mode, so the more specific one goes
    // after GM. MT-32 is a different, non-GM world; it is only meaningful on
    // its own, so it is sent solo (when it's the only flag set) and otherwise
    // skipped to avoid leaving the device half in MT-32, half in GM/GS/XG.
    const bool onlyMT32 = (m_flags == static_cast<unsigned>(MT32));

    // Ordered list of candidates; each sent only if its flag is enabled.
    const Flag order[] = { GM, GS, XG };

    bool bSentAny = false;
    auto emitOne = [&](Flag f) {
        if (bSentAny && m_delayMs && m_sleep)
            m_sleep(m_delayMs);
        m_send(BuildMessage(f));
        bSentAny = true;
    };

    if (onlyMT32) {
        emitOne(MT32);
    } else {
        for (Flag f : order) {
            if (m_flags & static_cast<unsigned>(f))
                emitOne(f);
        }
    }

    // Settle time AFTER the last reset, not just between resets. A module needs
    // a moment to act on a GM/GS/XG reset - the SC-55 family rebuilds its whole
    // patch state - and notes that arrive during that window can be swallowed
    // or land on a half-reset voice. The delay used to apply only between
    // messages, so with a single reset enabled it did nothing at all, which is
    // exactly what a user reported when raising it failed to change anything
    // (알로에, 2026-07-27). Safe to wait here now that the playback clock is
    // started after this returns (see MidiPlayer::play).
    if (bSentAny && m_delayMs && m_sleep)
        m_sleep(m_delayMs);
}
