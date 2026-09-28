#ifndef MLDFMPLAYER_H
#define MLDFMPLAYER_H

#include <QObject>
#include <QString>
#include <atomic>
#include <mutex>
#include <vector>

#include "mldfm.h"

// The OPM half of an MLD `.mdz`, as a player.
//
// **This one is ADDITIVE, and that is what makes it different from every other
// chip player here.** GybPlayer, ImsPlayer, MdxPlayer and VgmPlayer are
// alternatives - a song is played by exactly one of them, and JJoMeSynth picks
// with an `else if` chain. An MLD song that carries both halves needs its FM
// tracks to sound AT THE SAME TIME as its MIDI tracks, whether those are going
// to the internal SoundFont, an SC-55 or a real port. So this renders after
// that chain and mixes into whatever is already in the buffer.
//
// The balance exists because the answer is genuinely not in the file. On real
// hardware the X68000's FM output and the module's line output were two
// separate machines meeting at the listener's mixer, and nothing in a `.mdz`
// records where the knobs were. Measured over three songs the natural ratio
// runs from -11.2 dB to +5.1 dB, so a fixed choice would be wrong twice.
class MldFmPlayer : public QObject
{
    Q_OBJECT
public:
    explicit MldFmPlayer(QObject* parent = nullptr);

    // False when the song has no OPM or PCM tracks, which is the usual case -
    // 146 of the 158 songs here are MIDI only.
    bool loadFile(const QString& path);
    void unload();

    // play() starts from the top the first time and RESUMES after a pause;
    // stop() is what rewinds. It used to restart on every call, so pausing and
    // resuming a song put the FM half back at bar one while the MIDI half
    // carried on from where it was.
    void play();
    void stop();
    void pause();
    bool isPlaying() const { return m_playing.load(std::memory_order_relaxed); }
    bool isLoaded() const  { return m_loaded.load(std::memory_order_relaxed); }

    // Follows the MIDI half's seek, in the same song milliseconds.
    void seekMs(unsigned long ms);
    unsigned long positionMs() const;
    void setTempoScale(int percent);
    void setTranspose(int semitones);

    // 0 = the MIDI half alone, 50 = both at the level their own files ask for,
    // 100 = the FM half alone.
    void setBalance(int percent);
    int  balance() const { return m_balance.load(std::memory_order_relaxed); }
    float fmGain() const;
    float midiGain() const;

    void setVolume(int volume);              // 0-127, the master
    int  fmTrackCount() const { return m_fmTracks; }

    // Called from the audio thread. Mixes in; never clears the buffer.
    void renderAudio(float* output, unsigned int frameCount);

private:
    MldFm m_fm;
    mutable std::mutex m_lock;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loaded{false};
    bool m_started = false;      // m_fm has been started since the last stop
    std::atomic<int>  m_balance{50};
    std::atomic<int>  m_volume{100};
    std::vector<int16_t> m_scratch;
    int m_fmTracks = 0;
};

#endif
