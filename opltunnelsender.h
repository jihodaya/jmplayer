//
// opltunnelsender.h
//
// jmp side of the OPL register tunnel (see opltunnel.h). Collects the Nuked-OPL3
// register writes that happen on the miniaudio callback thread and, from a
// SEPARATE sender thread, packs them into MIDI SysEx and ships them to the
// jukebox (via a send callback wired to MidiPlayer::sendSysExMessage).
//
// Why a separate thread: midiOutLongMsg / midiOutPrepareHeader can block, and
// must never be called from the realtime audio callback. queueWrite()/reset()
// are lock-free and safe to call from the audio thread; the sender thread drains
// a single-producer/single-consumer ring every few ms.
//
#ifndef JMP_OPLTUNNELSENDER_H
#define JMP_OPLTUNNELSENDER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

class OplTunnelSender {
public:
    static OplTunnelSender& instance();

    // Wire the actual transport (MainWindow sets this to call
    // midiPlayer->sendSysExMessage). Called on the sender thread.
    void setSendCallback(std::function<void(const std::vector<unsigned char>&)> cb);

    // Start/stop tunnelling. Starting spins up the sender thread; stopping joins
    // it. While disabled, queueWrite()/reset() are cheap no-ops.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Audio thread: set the current song-position clock in ms (monotonic; from
    // the tick sequencer, see ImsPlayer::renderAudio). Every queued write is
    // stamped with the latest value, and writes sharing a stamp are shipped as
    // ONE timed batch (CmdBatch) the jukebox applies atomically at the right
    // time - this is what preserves tick timing across the serial link.
    void setNowMs(uint32_t ms) { m_nowMs.store(ms, std::memory_order_relaxed); }

    // Audio thread: queue one register write (reg = full 9-bit OPL3 register).
    // ALWAYS records the value in the local-state mirror (even while disabled),
    // so enabling the tunnel mid-song can snapshot the full chip state.
    void queueWrite(uint16_t reg, uint8_t val);

    // Any thread: jmp's virtual-stereo mode (1-9). The sender ships it as
    // CmdStereo whenever it changes and after every resync snapshot, so the
    // receiver's own pan policy mirrors jmp's setting (2026-07-16 - before
    // this, the receiver was fixed to center and jmp's stereo did nothing on
    // the Pi).
    void setStereoMode(int mode);

    // UI thread: master volume for the jukebox, 0-100 percent. Shipped by the
    // sender thread the same way the stereo mode is, so the wire sees one byte
    // when the slider settles instead of sixteen CC#7 messages the receiver
    // would discard anyway. Harmless against an older jukebox: its parser
    // ignores commands it does not know.
    void setVolume(int percent);

    // Audio thread: mark a chip reset (OPL song start / re-init). Ordered with
    // surrounding writes. Also clears the local-state mirror.
    void reset();

    // UI thread: wait (sleeping in small steps, up to nMaxMs) until the sender
    // thread has drained everything queued so far onto the wire. Used by
    // stop(): the gentle key-offs pushed by silenceAllVoices() must actually
    // REACH the device before suppression arms a resync - otherwise the
    // resync's discard-stale-entries step throws the key-offs away and ships
    // an abrupt CmdReset instead (2026-07-27 "정지 때 땡땡땡" root cause).
    void waitUntilDrained(unsigned nMaxMs);

    // UI thread (seek / whole-song load scans): while suppressed, queueWrite()
    // still mirrors every value (so the local-state snapshot stays correct) but
    // nothing is queued for the wire - a seek fast-forwards the entire song
    // through update() and would otherwise flood the 31250-baud link with tens
    // of seconds of backlog ("seek 후 소리가 밀려 한번에 들어온다"). Turning
    // suppression OFF arms a resync: the sender discards anything still queued
    // and ships CmdReset + a fresh full snapshot (the post-seek chip state)
    // before the resumed stream.
    void setSuppressed(bool suppressed);

    // Diagnostics (see MainWindow's title timer): writes actually shipped, and
    // writes dropped because the ring was full.
    unsigned sentCount() const { return m_cntSent.load(std::memory_order_relaxed); }
    unsigned droppedCount() const { return m_cntDropped.load(std::memory_order_relaxed); }

private:
    OplTunnelSender();
    ~OplTunnelSender();
    OplTunnelSender(const OplTunnelSender&) = delete;
    OplTunnelSender& operator=(const OplTunnelSender&) = delete;

    bool push(uint32_t entry); // false if dropped (ring full); stamps with m_nowMs
    void senderLoop();
    void flushBatch(std::vector<unsigned char>& sysex, unsigned& nWrites,
                    uint32_t stamp, uint32_t& prevStamp, bool& firstBatch);
    void sendReset();
    void sendSnapshot();  // sender thread: CmdReset + full m_localVal dump (CmdWrite)
    void sendKeepalive(); // sender thread: empty CmdBatch (keeps the Pi in live mode)
    void sendStereoMode(int mode); // sender thread: CmdStereo
    void sendVolume(int percent);  // sender thread: CmdVolume
    void clearShadow();

    static constexpr size_t RingSize = 1u << 15; // 32768 entries, power of 2
    static constexpr size_t RingMask = RingSize - 1;
    static constexpr uint32_t ResetMarker = 0xFFFFFFFFu;

    // Ring entry: low 32 bits = (reg<<8)|val or ResetMarker; high 32 = stamp ms.
    uint64_t m_ring[RingSize];
    std::atomic<size_t> m_head; // consumer (sender thread)
    std::atomic<size_t> m_tail; // producer (audio thread)
    std::atomic<uint32_t> m_nowMs;

    std::atomic<bool> m_enabled;
    std::atomic<bool> m_suppressed;
    std::atomic<bool> m_running;
    std::thread m_thread;

    std::function<void(const std::vector<unsigned char>&)> m_sendCb;
    std::atomic<bool> m_cbValid;

    // Per-register last-sent value, -1 = unknown. Redundant writes (same value
    // to the same register) are skipped: OPL registers are idempotent, so this
    // is lossless and cuts the byte rate a lot (crucial at 31250 baud). Cleared
    // on reset()/enable. Atomic because clearShadow() is called from
    // setEnabled() on the UI thread while queueWrite() reads/writes it from the
    // audio thread - was a plain int[], a genuine data race (could read a torn/
    // stale value and wrongly skip a write, i.e. drop a channel's state).
    std::atomic<int> m_lastVal[512];

    // Mirror of the LOCAL chip's registers (what jmp last wrote), -1 = never
    // written. Updated on every queueWrite even while the tunnel is DISABLED,
    // so setEnabled(true) can dump a full snapshot to the jukebox - without
    // this, instruments/pan registers written before the toggle never reach
    // the Pi (pan bits 0 = silent channels: the "only a few instruments" bug).
    std::atomic<int> m_localVal[512];

    std::atomic<unsigned> m_cntSent;
    std::atomic<unsigned> m_cntDropped;

    // Current stereo mode (1-9) + what the sender last shipped (-1 = never/
    // needs resend, e.g. right after a resync snapshot).
    std::atomic<int> m_stereoMode;
    std::atomic<int> m_stereoModeSent;

    // Master volume (0-100) + what the sender last shipped, same convention.
    std::atomic<int> m_volume;
    std::atomic<int> m_volumeSent;

    // Set on enable (and after long idle): the sender thread ships a CmdReset +
    // full local-state snapshot before the next stream, so the Pi chip is in
    // sync no matter when the tunnel was toggled or how long it idled out.
    std::atomic<bool> m_needResync;
};

#endif
