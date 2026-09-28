#ifndef MXDRV_H
#define MXDRV_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include "ym2151.h"
#include "msm6258.h"

/**
 * @brief Sharp X68000 사운드 드라이버 MXDRV 에뮬레이션 엔진
 */
class Mxdrv
{
public:
    Mxdrv();
    ~Mxdrv();

    void init(int sampleRate);
    void reset();

    bool loadMdx(const uint8_t* mdxData, size_t mdxSize, const uint8_t* pdxData = nullptr, size_t pdxSize = 0);

    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing && !m_paused; }

    void setVolume(float vol); // 0.0 to 1.0
    void setTempoScale(int scale); // 50 to 150

    // jmp's F12 virtual-stereo pattern, 1 (off) to 9. Applied ONLY to channels
    // the song leaves centred - see the note in mxdrv.cpp.
    void setStereoMode(int mode);
    void setKeyTranspose(int key);
    long keyOnCount() const { return m_keyOns; }
    // Sample position of every key-on, for the offline comparison harness.
    const std::vector<long>& keyOnTimes() const { return m_keyOnTimes; } // -6 to +6

    void render(float* buffer, int frameCount);
    void render16(int16_t* buffer, int frameCount);

    std::string getTitle() const { return m_title; }
    std::string getPdxName() const { return m_pdxName; }

    int getLoopCount() const { return m_currentLoop; }
    int getMaxLoops() const { return m_maxLoops; }
    void setMaxLoops(int loops) { m_maxLoops = loops; }

    int trackCount() const { return m_numTracks; }
    // For the channel monitor: 0-127 while the track is sounding, 0 otherwise.
    int voiceLevel(int track) const;
    int voiceNumber(int track) const;
    // Every voice number the file defines, in table order.
    std::vector<int> voiceNumbers() const;

    // Indicative only: MXDRV counts ticks, and MML convention puts 48 of
    // them in a quarter note. Timer B is the tick, so this is what a `t`
    // command in the source would have said.
    int currentBpm() const;

    uint32_t getElapsedMs() const;
    uint32_t getTotalMs() const { return m_totalMs; }
    // Runs the sequencer to the requested point without producing audio.
    void seekMs(uint32_t ms);

private:
    struct Track {
        uint8_t slotMask;
        bool active;
        const uint8_t* pc;
        const uint8_t* startPc;
        int waitTicks;
        int gateTicks;
        uint8_t q;          // gate ratio, as set by the q command
        uint8_t alg;        // algorithm, which decides the carrier operators
        uint8_t fbAlg;      // the raw (FB<<3)|ALG byte, kept for pan writes
        bool tie;           // the next note carries on rather than restarting
        bool keyedOn;       // is a note sounding on this channel right now
        uint8_t pmsAms;     // the channel's saved PMS/AMS, for the LFO switch

        // 0xEC, MML's `@m`: a per-track software pitch LFO. Names follow what
        // each field does; the driver calls them S0026 and S002a-S003e.
        uint8_t lfoWave;        // 1..4 = saw, square, triangle, random
        bool    lfoOn;
        int32_t lfoValue, lfoStep, lfoValueInit, lfoStepInit;
        int     lfoCounter, lfoPeriod, lfoCounterInit;
        int     lfoDelay, lfoDelayLeft;     // 0xE9
        int     pitch;          // the note's own pitch, before the LFO
        uint32_t lfoRandom;

        uint8_t opTL[4];    // the voice's own total level per operator
        int key;
        int length;
        int volume;
        int pan;
        int voice;
        int detune;
        bool isPcm;
        int loopStack[16];
        int loopCountStack[16];
        int loopStackPtr;
        int loopedCount;    // how many times this track has jumped back
    };

    void setupTracks();
    // Plays the song through with the chip silent, to find out how long it is.
    void measureTotalMs();
    void stepSequencer();
    void lfoRearm(Track& trk);
    void lfoTick(Track& trk, int trkIdx);
    void writePitch(Track& trk, int trkIdx);
    bool m_lfoEnabled = false;

    void parseTrack(Track& trk, int trkIdx);
    void sendVoice(int ch, int voiceIdx);
    const uint8_t* findVoice(int voiceNum) const;
    void applyVolume(int ch);

    int m_sampleRate;
    Ym2151 m_opm;
    // The X68000 mixes eight PCM channels (PCM8), one per PCM track. This was a
    // single Msm6258 that every track restarted in turn, so only the newest hit
    // was ever heard: the average level came out about right but the peaks did
    // not, because simultaneous drums never summed. Measured against the
    // reference render on ssf2_13, that cost 5.9 dB of crest.
    static const int kPcmVoices = 8;
    Msm6258 m_pcm[kPcmVoices];

    std::vector<uint8_t> m_mdxData;
    std::vector<uint8_t> m_pdxData;
    std::string m_title;
    std::string m_pdxName;

    static const int MAX_TRACKS = 16;   // eight FM plus eight PCM8
    Track m_tracks[MAX_TRACKS];
    int m_numTracks;
    long m_keyOns = 0;      // counted for the offline comparison harness
    std::vector<long> m_keyOnTimes;
    long m_samplePos = 0;

    // Voice table
    const uint8_t* m_trackBase;     // where the track offsets are measured from
    const uint8_t* m_voiceTable;
    const uint8_t* m_voiceTableEnd;
    size_t m_voiceTableSize;

    // PCM table
    struct PcmSample {
        const uint8_t* data;
        size_t size;
        int rate;
    };
    std::vector<PcmSample> m_pcmSamples;

    bool m_playing;
    bool m_paused;
    float m_volume;
    int m_tempoScale;
    int m_stereoMode = 1;
    uint8_t panBitsFor(int ch, int songPan) const;
    int m_keyTranspose;

    int m_currentLoop;
    int m_maxLoops;
    uint32_t m_elapsedMs;
    uint32_t m_totalMs;

    int m_timerB;
    int m_tickCounter;

    double m_samplesPerTick;    // exact; render() carries the fraction
    double m_tickAcc = 0.0;

    // Temp audio buffers
    std::vector<int32_t> m_leftBuf;
    std::vector<int32_t> m_rightBuf;
    // The eight PCM channels sum here, go through the output filter, and are
    // then added to the FM - see render().
    std::vector<int32_t> m_pcmLeft;
    std::vector<int32_t> m_pcmRight;

    // The X68000's output filter and level for that sum - see msm6258.h.
    X68PcmBus m_pcmBus;
};

#endif // MXDRV_H
