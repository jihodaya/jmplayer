#ifndef MT32SYNTH_H
#define MT32SYNTH_H

// Roland MT-32 / CM-32L emulation, in process, through munt's mt32emu library.
//
// Unlike Nuked-SC55 this is a *library*, not a separate program, so there is no
// pipe, no child process and no window to hunt for: MIDI goes in through a
// function call and PCM comes out through another. It is also LGPL-2.1, so the
// DLL ships with jmp instead of the user having to build a patched emulator.
//
// What cannot ship are the ROMs - they are Roland's - so they go in an
// MT32ROMs folder beside the executable, the same arrangement NukedSC55 uses.
//
// Threading. mt32emu documents playMsg()/playSysex() (the queueing forms, no
// timestamp) as needing no synchronisation with the rendering thread, so the
// sequencer thread calls them directly. What does need care is the synth's
// *lifetime*: Open()/Close() run on the UI thread while Render() may be inside
// the audio callback. Both build or tear down outside the lock and only swap
// pointers under it, so the audio thread never waits for a ROM to load.

#include <QString>
#include <QVector>

#include <atomic>
#include <mutex>
#include <vector>

namespace MT32Emu {
    class Synth;
    class SampleRateConverter;
    class ROMImage;
    class FileStream;
}

class Mt32Synth
{
public:
    // One usable machine: a control ROM and a PCM ROM that mt32emu accepts
    // together. `id` is munt's own machine identifier ("mt32_1_07", "cm32l_1_02")
    // and is what gets written to settings; `label` is for the UI.
    struct RomSet {
        QString id;
        QString label;
        QString controlPath;
        QString pcmPath;
    };

    // <exe folder>/MT32ROMs. Created on demand with a note in it, exactly as
    // Sc55Bridge does for its own folder, so the user has somewhere obvious to
    // put the files.
    static QString InstallDir();
    static void    EnsureInstallDir();

    // The entry in the output-device list. Bracketed like the other built-in
    // engines so it does not read as a system MIDI port - Windows may well
    // *also* be showing munt's own "MT-32 Synth Emulator" driver, which is a
    // different thing entirely.
    static QString DeviceLabel();

    // Every machine whose ROMs are present, in munt's own order (oldest first).
    static QVector<RomSet> ScanRomSets();

    // Empty when at least one complete set was found; otherwise a sentence
    // explaining what is missing, for a dialog.
    static QString UnavailableReason();

    Mt32Synth();
    ~Mt32Synth();

    Mt32Synth(const Mt32Synth&)            = delete;
    Mt32Synth& operator=(const Mt32Synth&) = delete;

    // Loads a machine. An empty id takes the first available - which is the
    // oldest MT-32 present, the closest thing to "what a 1980s file expects".
    bool    Open(const QString& romSetId = QString());
    void    Close();
    bool    IsOpen() const { return m_bOpen.load(std::memory_order_acquire); }
    QString CurrentRomId() const    { return m_sRomId; }
    QString CurrentRomLabel() const { return m_sRomLabel; }
    QString ErrorString() const     { return m_sError; }

    // --- sequencer thread ---
    void SendShort(unsigned char status, unsigned char data1, unsigned char data2);
    void SendSysEx(const std::vector<unsigned char>& framed);
    void AllSoundOff();

    // jmp's volume slider, 0-127, as the MT-32's own MASTER VOLUME.
    //
    // Not CC#7: that is per-part volume, which songs set themselves and which
    // this would be fighting. Master volume is the front-panel knob, it is what
    // the hardware shows on its display, and it is therefore the only way to
    // make the panel agree with the slider.
    void SetMasterVolume(int volume0to127);

    // --- audio thread ---
    // Interleaved stereo float at the device's rate; silence when closed.
    void Render(float* pOutput, unsigned int nFrames);

    // --- UI thread ---
    // The emulated 20-character LCD, and whether the MIDI MESSAGE lamp is lit.
    QString DisplayText() const;
    bool    MidiLed() const;

    // The device rate everything is resampled to. mt32emu runs at 32 kHz;
    // jmp's device is the OPL rate, so a converter always sits in between.
    static constexpr double kOutputSampleRate = 49716.0;

private:
    struct Loaded {
        MT32Emu::Synth*              pSynth   = nullptr;
        MT32Emu::SampleRateConverter* pConv   = nullptr;
        const MT32Emu::ROMImage*     pControl = nullptr;
        const MT32Emu::ROMImage*     pPCM     = nullptr;
        MT32Emu::FileStream*         pControlFile = nullptr;
        MT32Emu::FileStream*         pPCMFile     = nullptr;
    };

    static void Destroy(Loaded& L);

    mutable std::mutex m_lock;    // guards m_L; held only across pointer swaps
    Loaded             m_L;
    std::atomic<bool>  m_bOpen{false};

    QString m_sRomId;
    QString m_sRomLabel;
    QString m_sError;
};

#endif // MT32SYNTH_H
