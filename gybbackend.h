#ifndef GYBBACKEND_H
#define GYBBACKEND_H

// CcomposerBackend-derived GYB player. All OPL programming (instrument bank
// lookup, register writes, note frequency, volume scaling, pitch bend, rhythm
// mode) is delegated to AdPlug's CcomposerBackend so that GYB playback uses
// EXACTLY the same OPL path as IMS / SOP / ROL. The only thing we own is the
// GYB-specific file parsing (header, per-channel note streams, global events,
// embedded instrument slot table) and the tick scheduler that feeds
// AdPlug's NoteOn / NoteOff / SetVolume / ChangePitch / SetInstrument.

#include <cstring>  // composer.h uses strcmp/stricmp without including this itself
#include <adplug/composer.h>
#include <adplug/fprovide.h>
#include <QString>
#include <QStringList>
#include <QList>
#include <QByteArray>

class GybBackend : public CcomposerBackend
{
public:
    static CPlayer* factory(Copl* opl);

    explicit GybBackend(Copl* opl);
    ~GybBackend() override = default;

    // CPlayer interface
    bool        load(const std::string& filename, const CFileProvider& fp) override;
    bool        update() override;
    void        rewind(int subsong) override;
    float       getrefresh() override { return m_tickHz; }
    std::string gettype() override { return "GAYOBANG GYB"; }
    std::string gettitle() override { return m_title.toStdString(); }
    unsigned int getinstruments() override { return (unsigned int)m_slotNames.size(); }
    std::string  getinstrument(unsigned int n) override
    {
        if (n >= (unsigned int)m_slotNames.size()) return {};
        return m_slotNames[n].toStdString();
    }

    // CcomposerBackend pure-virtual stub (no-op for GYB)
    void frontend_rewind(int /*subsong*/) override {}

    std::string getvoiceinstrument(unsigned int voice) override;

    // GYB-specific accessors (used by the Qt wrapper)
    uint64_t currentTick() const { return m_currentTick; }
    int      totalTicks()  const { return m_totalSongTicks; }
    QString  bankName()    const { return m_bankName; }
    QString  title()       const { return m_title; }
    int      effectiveBpmX100() const { return m_effectiveBpmX100; }
    int      tbDiv()       const { return m_tbDiv; }
    int      basicTempo()  const { return m_basicTempo; }
    QStringList slotNames() const { return m_slotNames; }

    // Tick at which the song's silent lead-in ends, or 0 when it has none.
    //
    // A global event of 1000 at tick 0 marks a lead-in that GAYOBANG prints on
    // its score - "♩=1400" above the first few notes - but never sounds. It is
    // not a tempo: taken as one (ten times the base) the notes were heard as a
    // crushed flourish, and skipping the ticks outright started the song too
    // early. The range keeps its ordinary timing and is played with the key-on
    // bits held down, so the song proper begins exactly where the original does.
    unsigned long introSkipEndTick() const;

    // Ordinary tempo events sit near 100; the intro marker seen so far is 1000.
    static const int kIntroScaleThreshold = 500;

    // A bank the user registered with the BNK button. GYB stored this on the
    // player and never handed it down, so the button did nothing for GYB songs;
    // OkaBackend already had this pair.
    void    setExternalBankPath(const QString& path) { m_externalBankPath = path; }
    QString externalBankPath() const { return m_externalBankPath; }

    // User tempo/key controls
    void setUserTempoScale(int scale) { m_userTempoScale = scale; m_tickHz = tickHzAt(m_currentTick); }
    int  userTempoScale() const { return m_userTempoScale; }
    void setUserKeyTranspose(int semitones) { m_userKeyTranspose = semitones; }
    int  userKeyTranspose() const { return m_userKeyTranspose; }
    int  getCurrentBpm() const;

    // Refresh-rate at a given tick (honours global tempo events).
    float tickHzAt(uint64_t tick) const;

    // Returns -1 if no BNK was loaded for the current song.
    int  bnkPatchCount() const { return (int)m_slotToInstIndex.size(); }

    // Current GYB slot index (0-based, 0 = empty placeholder) that an OPL
    // voice is bound to. Used by the channel monitor to map voice-level
    // activity back to instrument slots even after program-change events.
    // Returns -1 if voice is unused.
    int  voiceSlot(int oplVoice) const
    {
        if (oplVoice < 0 || oplVoice >= 18) return -1;
        return m_voiceSlot[oplVoice];
    }

    // Consume the per-voice note-onset level since the last call (for the level
    // meter). Drives the OKA-style onset+decay meter so the bars bounce per note.
    int consumeVoiceAttack(int voice)
    {
        if (voice < 0 || voice >= 18) return 0;
        int a = m_voiceAttack[voice];
        m_voiceAttack[voice] = 0;
        return a;
    }

    // Pre-scanned note events for the piano-roll window. Populated by
    // parseHeaderAndChannels() so the roll can show the song's note layout
    // without waiting for live playback.
    struct RollNote { uint64_t tick; int channel; int pitch; };
    const QList<RollNote>& rollNotes() const { return m_rollNotes; }

private:
    struct EventEntry { int tick; int value; };
    struct TrackState {
        int channelId;
        int oplVoice;
        int curPos;
        int noteEnd;
        int waitTicks;
        int activeNote;
        bool done;
        int program;            // GYB instrument slot index (0-based, 0 = empty placeholder)
        int volume;
        int pitchBend;
        QList<EventEntry> progEvents;
        QList<EventEntry> volEvents;
        QList<EventEntry> pitchEvents;
        int progIdx, volIdx, pitchIdx;
    };

    QByteArray m_rawData;
    QList<TrackState> m_tracks;
    QList<EventEntry> m_globalEvents;

    // GYB slot table (slot 0 is GAYOBANG's empty placeholder, never targeted by
    // a program-change). We keep the full table here for indexing convenience,
    // and let the Qt wrapper hide slot 0 from the channel monitor.
    QStringList m_slotNames;
    QList<int>  m_slotToInstIndex; // slot index → CcomposerBackend instrument index, -1 if missing

    // The 28 OPL parameter bytes of each embedded instrument record - what the
    // song actually sounds like, independent of any external bank.
    static const int kEmbeddedParamLen = 28;
    QList<QByteArray> m_instParams;
    bool loadEmbeddedPatches();
    int  builtinInstIndex(int voice);
    int  m_builtinInst[6] = { -1, -1, -1, -1, -1, -1 };

    // Volume/instrument wrappers that honour GAYOBANG's additive rule.
    void SetChannelVolume(int voice, uint8_t volume);
    void FixRhythmFrequency(int voice);
    void NoteInstrument(int voice, int slot, int instIdx);

    QString m_externalBankPath;
    int         m_voiceSlot[18];   // OPL voice → GYB slot index currently programmed
    bool        m_voiceAdditive[18] = {false}; // patch uses additive connection
    uint8_t     m_voiceModKsltl[18] = {0};     // its modulator KSL/TL, unscaled
    int         m_voiceAttack[18]; // per-voice note-onset level for the level meter

    QString m_title;
    QString m_bankName;
    float   m_tickHz;          // current Hz (refreshed on tempo event)
    int     m_basicTempo;      // header @0x32 fallback
    int     m_tbDiv;           // header @0x28
    int     m_effectiveBpmX100;// header @0x34 = BPM × 100 (Iyagi tempo dial)
    int     m_keyTranspose;    // 60 - header[0x2E] (semitone offset for note-on)
    int     m_userTempoScale;  // user-defined tempo scale (50% ~ 150%)
    int     m_userKeyTranspose;// user-defined key transpose offset
    int     m_totalSongTicks;
    uint64_t m_currentTick;

    // Cached rhythm-mode state — true means GYB uses ch 6-10 as percussive
    // OPL voices (bd/sd/tom/cym/hh) instead of melodic 0..10.
    bool m_rhythmMode;

    QString m_songFilename;

    // Pre-scanned roll notes (filled by parseHeaderAndChannels, consumed by
    // the piano-roll window via the Qt wrapper).
    QList<RollNote> m_rollNotes;

    int  globalTempoScale(uint64_t tick) const;
    bool advanceOneTick();
    void applyChannelEvents(TrackState& t, int currentTick);
    int  processChunk(TrackState& t, uint8_t cmd, uint8_t dur);
    bool resolveBnkPatches(const CFileProvider& fp, const std::string& songFilename);
    void parseHeaderAndChannels();
};

#endif // GYBBACKEND_H
