//
// midireset.h
//
// jmp - "reset the sound module before each new song" feature.
//
// When jmp drives an EXTERNAL MIDI device (a hardware synth, or this project's
// mt32-pi over the Pico serial bridge), the device remembers everything a song
// set up - instruments, pan, reverb, tuning, controllers. If the next song
// doesn't re-set all of that (and 96% of this library's files send no reset of
// their own - measured), the previous song's state bleeds into it and it plays
// with the wrong sound. jmp's own initializeChannelState() only resets jmp's
// INTERNAL variables; the All Notes/Sound Off it sends merely silences ringing
// notes, it does not clear accumulated module state. This module fills that
// gap: it emits the standard reset SysEx messages at the start of every song.
//
// Design (matches how real MIDI players do it, e.g. Falcosoft): the reset is
// aimed at the DEVICE, which never changes between songs, so it is NOT chosen
// by (unreliable) per-file format auto-detection. Instead a fixed, user-chosen
// set of resets is sent every time. The safe default sends GM then GS (this
// project's target, mt32-pi, runs a GM/GS-compatible SoundFont); XG and MT-32
// are opt-in for other hardware. Order matters - resets overwrite each other's
// device mode, so the more specific one must come last (GM first, then GS/XG).
//
// Self-contained: depends only on <vector>/<functional>. The host wires a send
// callback (jmp: MidiPlayer::sendRawSysEx) and calls SendResets() from
// MidiPlayer::play()'s new-song branch.
//
#ifndef MIDIRESET_H
#define MIDIRESET_H

#include <cstdint>
#include <functional>
#include <vector>

class MidiReset {
public:
    // Which resets to emit. OR-combinable; sent in a fixed device-mode-safe
    // order regardless of the bit order here (GM, then GS, then XG; MT-32 is
    // a separate world and only sent when it's the ONLY thing selected).
    enum Flag : unsigned {
        None = 0,
        GM   = 1u << 0, // F0 7E 7F 09 01 F7            (GM System On)
        GS   = 1u << 1, // F0 41 10 42 12 40 00 7F 00 41 F7 (Roland GS Reset)
        XG   = 1u << 2, // F0 43 10 4C 00 00 7E 00 F7   (Yamaha XG System On)
        MT32 = 1u << 3, // F0 41 10 16 12 7F 00 01 00 F7 (MT-32 master reset)

        // Sensible default for this project (mt32-pi = GM/GS SoundFont).
        DefaultGMGS = GM | GS,
    };

    MidiReset();

    // Host transport: called once per reset SysEx (jmp wires this to
    // MidiPlayer::sendRawSysEx). Each vector is a complete F0..F7 message.
    void SetSendCallback(std::function<void(const std::vector<uint8_t>&)> cb);

    // Master on/off and which resets are enabled (persisted by the host).
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    void SetFlags(unsigned flags) { m_flags = flags; }
    unsigned Flags() const { return m_flags; }

    // Milliseconds to wait after EACH reset message - both between successive
    // resets and after the final one, before the song's own events go out. A
    // module needs a moment to act on a reset (Roland's GS documentation asks
    // for roughly 50 ms, and SC-55 emulators such as Nuked-SC55 run the real
    // firmware, so they want it too); notes arriving inside that window can be
    // swallowed or land on a half-reset voice. Software synths are ready
    // immediately and can use 0. Host supplies the sleep function so this
    // module stays framework-free (jmp passes a QThread::msleep shim).
    //
    // Costs (delay x number of resets) of extra silence before the song starts.
    // It does NOT affect playback timing: MidiPlayer::play() starts its clock
    // after SendResets() returns.
    static constexpr unsigned DefaultDelayMs = 50;

    void SetInterMessageDelayMs(unsigned ms) { m_delayMs = ms; }
    void SetSleepFunction(std::function<void(unsigned)> sleepMs) { m_sleep = std::move(sleepMs); }

    // Emit the enabled resets, in device-mode-safe order, via the send
    // callback. No-op when disabled, when no flags are set, or when no send
    // callback is wired. Called from MidiPlayer::play() at each new song.
    void SendResets() const;

    // Build a single reset message (exposed for a manual "reset now" action /
    // tests). Returns an empty vector for None.
    static std::vector<uint8_t> BuildMessage(Flag which);

private:
    bool m_enabled;
    unsigned m_flags;
    unsigned m_delayMs;
    std::function<void(const std::vector<uint8_t>&)> m_send;
    std::function<void(unsigned)> m_sleep;
};

#endif // MIDIRESET_H
