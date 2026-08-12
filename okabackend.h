#ifndef OKABACKEND_H
#define OKABACKEND_H

#include <cstring>
#include <vector>
#include <adplug/composer.h>
#include <adplug/fprovide.h>
#include <QString>
#include <QStringList>
#include <QList>
#include <QByteArray>

struct OkaMidiEvent {
    unsigned long absoluteTick;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    bool isMeta;
    int metaType;
    QByteArray metaData;
};

class OkaBackend : public CcomposerBackend
{
public:
    static CPlayer* factory(Copl* opl);

    explicit OkaBackend(Copl* opl);
    ~OkaBackend() override = default;

    // CPlayer interface
    bool        load(const std::string& filename, const CFileProvider& fp) override;
    bool        load(const std::string& filename);
    bool        update() override;
    void        rewind(int subsong) override;
    float       getrefresh() override { return m_tickHz; }
    std::string gettype() override { return "Oksori OKA OPL"; }
    std::string gettitle() override { return m_title.toStdString(); }
    unsigned int getinstruments() override { return (unsigned int)m_slotNames.size(); }
    std::string  getinstrument(unsigned int n) override
    {
        if (n >= (unsigned int)m_slotNames.size()) return {};
        return m_slotNames[n].toStdString();
    }

    // CcomposerBackend pure-virtual stub (no-op for OKA)
    void frontend_rewind(int /*subsong*/) override {}

    // OKA-specific accessors
    uint64_t currentTick() const { return m_currentTick; }
    int      totalTicks()  const { return m_totalSongTicks; }
    QString  bankName()    const { return m_bankName; }
    QString  title()       const { return m_title; }
    QStringList slotNames() const { return m_slotNames; }

    void setUserTempoScale(int scale) { m_userTempoScale = scale; m_tickHz = calculateTickHz(m_currentTick); }
    int  userTempoScale() const { return m_userTempoScale; }
    void setUserKeyTranspose(int semitones) { m_userKeyTranspose = semitones; }
    int  userKeyTranspose() const { return m_userKeyTranspose; }
    int  getCurrentBpm() const;

    // External OPL bank override (top-priority over STANDARD.BNK). Empty = default.
    void    setExternalBankPath(const QString& path) { m_externalBankPath = path; }
    QString externalBankPath() const { return m_externalBankPath; }

    std::string getvoiceinstrument(unsigned int voice) override;

    // voice status mapping
    int voiceSlot(int oplVoice) const
    {
        if (oplVoice < 0 || oplVoice >= 18) return -1;
        return m_voiceSlot[oplVoice];
    }

    const QList<OkaMidiEvent>& getEvents() const { return m_events; }

    // Consume the per-voice note-onset velocity since the last call (for the
    // level meter). OKA MIDI has no note-offs (monophonic, voices stay keyon),
    // so the meter is driven by note onsets + decay instead of keyon state.
    int consumeVoiceAttack(int voice)
    {
        if (voice < 0 || voice >= 18) return 0;
        int a = m_voiceAttack[voice];
        m_voiceAttack[voice] = 0;
        return a;
    }

private:
    struct TempoChange {
        unsigned long tick;
        unsigned long tempo; // MPQN
    };

    QByteArray m_rawData;
    QList<OkaMidiEvent> m_events;
    QList<TempoChange> m_tempoMap;
    int m_eventIdx;

    QStringList m_slotNames;
    QList<int>  m_slotToInstIndex;
    int         m_voiceSlot[18];   // OPL voice -> OKA slot index currently programmed
    int         m_initialProgram[18]; // OPL voice -> initial OKA slot index
    int         m_voiceNote[18];   // Currently sounding MIDI note per voice (-1 if none)
    int         m_channelVolume[16]; // CC7 channel volume per MIDI channel (default 100)
    int         m_lastVel[16];       // last note-on velocity per MIDI channel (default 100)
    int         m_voiceAttack[18];   // per-voice note-onset velocity for the level meter

    QString m_title;
    QString m_bankName;
    QString m_externalBankPath;

    // The 28 OPL parameter bytes of each embedded instrument record - the song
    // carries its own bank (see GybBackend::loadEmbeddedPatches).
    static const int kEmbeddedParamLen = 28;
    QList<QByteArray> m_instParams;
    bool loadEmbeddedPatches();

    // Volume / instrument / rhythm wrappers that follow NORE45's driver.
    void SetChannelVolume(int voice, uint8_t volume);
    void NoteInstrument(int voice, int slot, int instIdx);
    void FixRhythmFrequency(int voice);
    bool    m_voiceAdditive[18] = {false};
    uint8_t m_voiceModKsltl[18] = {0};
    float   m_tickHz;
    unsigned long m_ticksPerQuarter;
    unsigned long m_currentTempo; // microseconds per quarter note
    int     m_userTempoScale;
    int     m_userKeyTranspose;
    int     m_totalSongTicks;
    uint64_t m_currentTick;

    bool parseMidiData(const QByteArray& midiData);
    bool resolveBnkPatches(const std::string& songFilename);
    float calculateTickHz(uint64_t tick) const;
    unsigned long tempoAtTick(uint64_t tick) const;
    bool advanceOneTick();
};

#endif // OKABACKEND_H
