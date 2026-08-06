//
// opltunnelsender.cpp
//
#include "opltunnelsender.h"
#include "opltunnel.h"

#include <chrono>

OplTunnelSender& OplTunnelSender::instance() {
    static OplTunnelSender s;
    return s;
}

OplTunnelSender::OplTunnelSender()
    : m_head(0), m_tail(0), m_nowMs(0), m_enabled(false), m_suppressed(false),
      m_running(false), m_cbValid(false),
      m_cntSent(0), m_cntDropped(0),
      m_stereoMode(1), m_stereoModeSent(-1),
      m_volume(100), m_volumeSent(-1), m_needResync(false) {
    clearShadow();
    for (int i = 0; i < 512; ++i)
        m_localVal[i].store(-1, std::memory_order_relaxed);
}

void OplTunnelSender::clearShadow() {
    for (int i = 0; i < 512; ++i)
        m_lastVal[i].store(-1, std::memory_order_relaxed);
}

OplTunnelSender::~OplTunnelSender() {
    setEnabled(false);
}

void OplTunnelSender::setSendCallback(std::function<void(const std::vector<unsigned char>&)> cb) {
    m_sendCb = std::move(cb);
    m_cbValid.store(m_sendCb != nullptr, std::memory_order_release);
}

void OplTunnelSender::setEnabled(bool enabled) {
    if (enabled == m_enabled.load(std::memory_order_relaxed))
        return;

    if (enabled) {
        // Drop any stale queue contents + shadow from a previous session, and
        // have the sender thread ship a CmdReset + full local-state snapshot
        // before anything else (see m_needResync / sendSnapshot()) - without
        // it, registers written BEFORE the toggle (instrument patches, 0xC0
        // pan bits) never reach the Pi; reset-cleared pan bits leave those
        // channels totally silent ("only a few instruments play").
        m_head.store(m_tail.load(std::memory_order_acquire), std::memory_order_release);
        clearShadow();
        m_needResync.store(true, std::memory_order_release);

        m_enabled.store(true, std::memory_order_release);
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&OplTunnelSender::senderLoop, this);
    } else {
        m_enabled.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable())
            m_thread.join();
    }
}

bool OplTunnelSender::push(uint32_t entry) {
    // Single-producer. Drop the write if the ring is full rather than block the
    // audio thread (a full ring means the link is saturated - losing a couple of
    // register writes is preferable to an audio glitch).
    const size_t tail = m_tail.load(std::memory_order_relaxed);
    const size_t head = m_head.load(std::memory_order_acquire);
    if (tail - head >= RingSize)
        return false; // full - dropped
    const uint64_t stamped = ((uint64_t)m_nowMs.load(std::memory_order_relaxed) << 32) | entry;
    m_ring[tail & RingMask] = stamped;
    m_tail.store(tail + 1, std::memory_order_release);
    return true;
}

void OplTunnelSender::queueWrite(uint16_t reg, uint8_t val) {
    reg &= 0x1FF;

    // Always mirror the local chip state (cheap), even while disabled - this is
    // what makes the enable-time snapshot possible.
    m_localVal[reg].store((int)val, std::memory_order_relaxed);

    if (!m_enabled.load(std::memory_order_relaxed))
        return;
    // Suppressed (seek fast-forward / load-time full-song scan): mirror only,
    // nothing on the wire - setSuppressed(false) resyncs from the mirror.
    if (m_suppressed.load(std::memory_order_relaxed))
        return;
    // Skip redundant writes (lossless - OPL registers are idempotent). This is
    // the main defence against saturating the 31250-baud link. Only mark the
    // register as sent if it was actually queued: if push() dropped it (ring
    // full), leaving m_lastVal unchanged means a later write of the same value
    // still gets sent, so a drop can't permanently corrupt that register.
    if (m_lastVal[reg].load(std::memory_order_relaxed) == (int)val)
        return;
    if (push(((uint32_t)reg << 8) | val))
        m_lastVal[reg].store((int)val, std::memory_order_relaxed);
    else
        m_cntDropped.fetch_add(1, std::memory_order_relaxed);
}

void OplTunnelSender::reset() {
    // The local chip is being deep-reset: every mirrored register is stale.
    for (int i = 0; i < 512; ++i)
        m_localVal[i].store(-1, std::memory_order_relaxed);

    if (!m_enabled.load(std::memory_order_relaxed))
        return;
    // Suppressed resets (rewinds inside a seek/scan) stay local: the Pi keeps
    // playing the old state until the unsuppress-resync replaces it wholesale.
    if (m_suppressed.load(std::memory_order_relaxed))
        return;
    clearShadow(); // after a chip reset every register is unknown again
    push(ResetMarker);
}

void OplTunnelSender::waitUntilDrained(unsigned nMaxMs) {
    if (!m_enabled.load(std::memory_order_relaxed))
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(nMaxMs);
    while (m_head.load(std::memory_order_acquire) != m_tail.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void OplTunnelSender::setSuppressed(bool suppressed) {
    if (suppressed == m_suppressed.load(std::memory_order_relaxed))
        return;
    if (suppressed) {
        m_suppressed.store(true, std::memory_order_release);
        return;
    }
    m_suppressed.store(false, std::memory_order_release);
    if (m_enabled.load(std::memory_order_relaxed)) {
        // The wire missed everything since suppression began - the dedup
        // shadow no longer reflects the Pi's chip, and anything still queued
        // predates the seek. Arm a resync: the sender discards stale queued
        // entries and ships CmdReset + the CURRENT mirror as a snapshot.
        clearShadow();
        m_needResync.store(true, std::memory_order_release);
    }
}

void OplTunnelSender::sendReset() {
    if (!m_cbValid.load(std::memory_order_acquire))
        return;
    std::vector<unsigned char> sx = {
        0xF0, OplTunnel::ManufacturerNonCommercial, OplTunnel::SubID,
        OplTunnel::CmdReset, 0xF7
    };
    m_sendCb(sx);
}

void OplTunnelSender::setStereoMode(int mode) {
    if (mode < 1 || mode > 9)
        mode = 1;
    m_stereoMode.store(mode, std::memory_order_relaxed);
    // senderLoop notices m_stereoMode != m_stereoModeSent and ships CmdStereo.
}

void OplTunnelSender::sendStereoMode(int mode) {
    if (!m_cbValid.load(std::memory_order_acquire))
        return;
    std::vector<unsigned char> sx = {
        0xF0, OplTunnel::ManufacturerNonCommercial, OplTunnel::SubID,
        OplTunnel::CmdStereo, (unsigned char)(mode & 0x7F), 0xF7
    };
    m_sendCb(sx);
}

void OplTunnelSender::setVolume(int percent) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    m_volume.store(percent, std::memory_order_relaxed);
    // senderLoop notices m_volume != m_volumeSent and ships CmdVolume.
}

void OplTunnelSender::sendVolume(int percent) {
    if (!m_cbValid.load(std::memory_order_acquire))
        return;
    std::vector<unsigned char> sx = {
        0xF0, OplTunnel::ManufacturerNonCommercial, OplTunnel::SubID,
        OplTunnel::CmdVolume, (unsigned char)(percent & 0x7F), 0xF7
    };
    m_sendCb(sx);
}

void OplTunnelSender::sendKeepalive() {
    if (!m_cbValid.load(std::memory_order_acquire))
        return;
    // Empty timed batch (dt = 0): advances nothing, applies nothing - its only
    // job is serial traffic so the jukebox's live-mode idle timer doesn't fire
    // mid-song during write-free stretches (long held chords under dedup).
    std::vector<unsigned char> sx = {
        0xF0, OplTunnel::ManufacturerNonCommercial, OplTunnel::SubID,
        OplTunnel::CmdBatch, 0x00, 0x00, 0xF7
    };
    m_sendCb(sx);
}

void OplTunnelSender::sendSnapshot() {
    if (!m_cbValid.load(std::memory_order_acquire))
        return;

    // CmdReset first (clean chip), then every register jmp has written, as
    // immediate CmdWrite messages (<=40 writes each: the WHOLE SysEx must stay
    // <=128 bytes or the Pico's BurstTransmitter silently drops it - see
    // senderLoop's MaxWritesPerSysEx comment; 5 framing + 3x40 + F7 = 126).
    // Ascending order lands A0 (freq) before B0 (key-on) before C0 (pan).
    // m_lastVal is NOT touched here (it belongs to the audio thread) -
    // consistency holds because the snapshot ships exactly the mirrored
    // values the audio thread thinks were sent.
    sendReset();

    std::vector<unsigned char> sx;
    unsigned n = 0;
    for (int i = 0; i < 512; ++i) {
        int v = m_localVal[i].load(std::memory_order_relaxed);
        if (v < 0)
            continue;
        // 2026-07-27 ("정지/재생 때 땡땡땡"): NEVER ship key-on state in a
        // snapshot. A snapshot's job is instrument/pan/mode setup; if the
        // mirrored B0-B8 values carry key-on bit 5 (or 0xBD carries drum key
        // bits) from the moment the mirror was captured, applying the snapshot
        // RETRIGGERS those notes/drums on the Pi - one audible "땡" per
        // resync. Mask them out: real notes are (re)keyed by the live stream.
        const int base = i & 0xFF;
        if (base >= 0xB0 && base <= 0xB8)
            v &= ~0x20;             // melodic key-on off
        else if (i == 0xBD)
            v &= 0xE0;              // keep AM/VIB depth + rhythm mode, clear drum keys
        if (n == 0) {
            sx.clear();
            sx.push_back(0xF0);
            sx.push_back(OplTunnel::ManufacturerNonCommercial);
            sx.push_back(OplTunnel::SubID);
            sx.push_back(OplTunnel::CmdWrite);
        }
        uint8_t packed[3];
        OplTunnel::Pack((uint16_t)i, (uint8_t)v, packed);
        sx.push_back(packed[0]);
        sx.push_back(packed[1]);
        sx.push_back(packed[2]);
        if (++n >= 40) { // 128-byte Pico limit - see comment above
            sx.push_back(0xF7);
            m_sendCb(sx);
            m_cntSent.fetch_add(n, std::memory_order_relaxed);
            n = 0;
        }
    }
    if (n > 0) {
        sx.push_back(0xF7);
        m_sendCb(sx);
        m_cntSent.fetch_add(n, std::memory_order_relaxed);
    }
}

// Emit the accumulated batch as one CmdBatch SysEx. The batch's dt (ms since
// the previously EMITTED batch) is patched into the two reserved payload bytes.
// Continuations of an oversized same-stamp batch naturally get dt=0.
void OplTunnelSender::flushBatch(std::vector<unsigned char>& sysex, unsigned& nWrites,
                                 uint32_t stamp, uint32_t& prevStamp, bool& firstBatch) {
    if (nWrites == 0)
        return;

    uint32_t dt = 0;
    if (!firstBatch && stamp > prevStamp)
        dt = stamp - prevStamp;
    if (dt > 16383)
        dt = 16383;
    prevStamp = stamp;
    firstBatch = false;

    sysex[4] = (unsigned char)(dt & 0x7F);        // dtLo7
    sysex[5] = (unsigned char)((dt >> 7) & 0x7F); // dtHi7
    sysex.push_back(0xF7);
    if (m_cbValid.load(std::memory_order_acquire))
        m_sendCb(sysex);
    m_cntSent.fetch_add(nWrites, std::memory_order_relaxed);
    sysex.clear();
    nWrites = 0;
}

void OplTunnelSender::senderLoop() {
    // Cap writes per SysEx so the WHOLE message stays under the Pico bridge's
    // REAL limit. 2026-07-15 ROOT CAUSE of "instruments deterministically
    // missing on the Pi while key-ons/VU still work": the Pico's parser allows
    // SysEx up to 512 bytes, but its BurstTransmitter (code.py, buffer
    // bytearray(128)) SILENTLY DROPS any single message longer than 128 bytes.
    // The old 120-write cap (367B) targeted the parser limit, so every
    // song-start instrument-init batch and every snapshot chunk was thrown
    // away at the Pico, while small per-tick batches (key-ons) passed - hence
    // VU bars lit with no sound, same channels every time. 40 writes = 7
    // framing + 120 data = 127 bytes <= 128. (Pico stays unmodified.)
    constexpr unsigned MaxWritesPerSysEx = 40;

    std::vector<unsigned char> sysex;
    unsigned nWrites = 0;
    uint32_t batchStamp = 0;  // stamp of the batch being accumulated
    uint32_t prevStamp = 0;   // stamp of the last EMITTED batch
    bool firstBatch = true;   // next batch anchors the receiver clock (dt=0)

    auto beginBatch = [&](uint32_t stamp) {
        sysex.clear();
        sysex.push_back(0xF0);
        sysex.push_back(OplTunnel::ManufacturerNonCommercial);
        sysex.push_back(OplTunnel::SubID);
        sysex.push_back(OplTunnel::CmdBatch);
        sysex.push_back(0); // dtLo7 - patched in flushBatch
        sysex.push_back(0); // dtHi7
        batchStamp = stamp;
    };

    auto lastSend = std::chrono::steady_clock::now();
    // Keepalives are gated on the SEQUENCER CLOCK advancing, not on ring
    // traffic: setNowMs() is called every tick while a song actually plays
    // (even when dedup produces zero writes), and freezes the moment playback
    // stops/pauses/ends. That way a held chord still gets keepalives, but a
    // finished song goes quiet immediately and the jukebox's own live-idle
    // timer hands control back to it naturally (2026-07-15 user request:
    // "끝나면 정상적으로 종료").
    auto lastClockChange = lastSend;
    uint32_t lastClockVal = m_nowMs.load(std::memory_order_relaxed);

    while (m_running.load(std::memory_order_acquire)) {
        size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);

        // Resync (on enable, resuming after the Pi idled back to jukebox mode,
        // or after a seek's suppression window): reset + full snapshot BEFORE
        // the resumed stream. Entries still queued predate the resync trigger
        // and are already reflected in the mirror the snapshot ships - sending
        // them AFTER the snapshot would replay stale state on top of it, so
        // discard them.
        if (head != tail && m_needResync.exchange(false, std::memory_order_acq_rel)) {
            head = tail;
            m_head.store(head, std::memory_order_release);
            sendSnapshot();
            m_stereoModeSent.store(-1, std::memory_order_relaxed); // resend below
            m_volumeSent.store(-1, std::memory_order_relaxed);     // ditto
            firstBatch = true; // receiver re-anchors on the next batch
        }

        // Ship the virtual-stereo mode whenever it changed (or after a resync)
        // BEFORE this cycle's batches, so pan policy is in place first.
        {
            const int mode = m_stereoMode.load(std::memory_order_relaxed);
            if (mode != m_stereoModeSent.load(std::memory_order_relaxed)) {
                sendStereoMode(mode);
                m_stereoModeSent.store(mode, std::memory_order_relaxed);
            }
        }

        // Same treatment for the master volume: one byte, only when it changed.
        {
            const int vol = m_volume.load(std::memory_order_relaxed);
            if (vol != m_volumeSent.load(std::memory_order_relaxed)) {
                sendVolume(vol);
                m_volumeSent.store(vol, std::memory_order_relaxed);
            }
        }

        const bool bHadEntries = (head != tail);

        while (head != tail) {
            const uint64_t stampedEntry = m_ring[head & RingMask];
            head++;
            const uint32_t e = (uint32_t)stampedEntry;
            const uint32_t stamp = (uint32_t)(stampedEntry >> 32);

            if (e == ResetMarker) {
                flushBatch(sysex, nWrites, batchStamp, prevStamp, firstBatch);
                sendReset();
                firstBatch = true; // receiver re-anchors on the next batch
                m_needResync.store(false, std::memory_order_release); // fresh reset supersedes
            } else {
                // A new stamp closes the current batch (one batch = one tick).
                if (nWrites > 0 && stamp != batchStamp)
                    flushBatch(sysex, nWrites, batchStamp, prevStamp, firstBatch);
                if (nWrites == 0)
                    beginBatch(stamp);

                const uint16_t reg = (uint16_t)((e >> 8) & 0x1FF);
                const uint8_t val = (uint8_t)(e & 0xFF);
                uint8_t packed[3];
                OplTunnel::Pack(reg, val, packed);
                sysex.push_back(packed[0]);
                sysex.push_back(packed[1]);
                sysex.push_back(packed[2]);
                if (++nWrites >= MaxWritesPerSysEx)
                    flushBatch(sysex, nWrites, batchStamp, prevStamp, firstBatch);
            }
        }
        m_head.store(head, std::memory_order_release);

        // Ship whatever accumulated this cycle.
        flushBatch(sysex, nWrites, batchStamp, prevStamp, firstBatch);

        const auto now = std::chrono::steady_clock::now();
        const uint32_t clockNow = m_nowMs.load(std::memory_order_relaxed);
        if (clockNow != lastClockVal) {
            lastClockVal = clockNow;
            lastClockChange = now;
        }

        if (bHadEntries) {
            lastSend = now;
        } else if (now - lastClockChange < std::chrono::seconds(2)) {
            // Sequencer clock still advancing = a song is genuinely playing;
            // keep the Pi's live mode alive through write-free stretches
            // (held chords under dedup produce nothing).
            if (now - lastSend >= std::chrono::seconds(1)) {
                sendKeepalive();
                lastSend = now;
            }
        } else {
            // Stopped / paused / song ended: go silent so the Pi's live-idle
            // timer (3s) hands control back to its own jukebox naturally.
            // The Pi may then play its own things to the chip, so arm a
            // resync: when our stream resumes, a CmdReset + full snapshot
            // rebuilds its chip state first (a song-start CmdReset in the
            // stream supersedes this - see the ResetMarker branch above).
            m_needResync.store(true, std::memory_order_release);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // Final drain on stop.
    flushBatch(sysex, nWrites, batchStamp, prevStamp, firstBatch);
}
