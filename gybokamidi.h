// Playing a .GYB / .OKA through MIDI instead of OPL, and deciding which MIDI
// instrument each of the song's OPL slots becomes.
//
// GAYOBANG and NORE45 could both do this - sound-source mode 7 drove an MPU-401
// and their `악기변경` dialog let the operator reassign a slot while the song
// played. This is the same idea with two differences that come from what the
// port can measure and the DOS program could not:
//
//  * the original started every slot at tone 1 and made you assign all of them;
//    here a slot's name is matched against a rule set first, which lands 89 % of
//    the library's actual slot uses;
//  * the original stored its choice in the song file. That data is still there
//    and is read as the strongest default (see mt32map.h), but this player
//    writes its own choices to a sidecar `.ini` beside the song instead of
//    modifying the original file.
//
// The format reading is jmpconv's (convert/), the policy is jmp's.
#pragma once

#include <QString>
#include <QByteArray>
#include <QVector>

namespace gybokamidi {

// Where a row's current assignment came from. Shown in the F5 dialog, because
// the three carry very different confidence.
enum class Origin {
    SongFile,     // the song's own flag byte - a human's choice, kept as-is
    NameMatch,    // matched the OPL patch name against gmmap's rules
    Unmatched,    // nothing recognised it; this is what needs an ear
    User,         // edited here, or restored from the sidecar
};

// Which module the assignments are written for.
//
// The two are different instrument sets, not two names for one: GM program 0 is
// a piano everywhere, MT-32 tone 1 is `Acou Piano1` and tone 12 is `Elec Org4`,
// which GM has no equivalent for. A song therefore carries a choice for each,
// kept side by side so flipping the toggle does not throw the other away.
//
// This is also what the song files themselves assumed - GAYOBANG's MIDI mode
// drove an MPU-401 with an MT-32 on the end, and the tone numbers in their flag
// bytes are MT-32 numbers (mt32map.h). Until now those were approximated into
// GM; picking MT-32 here plays them as written.
enum class Target {
    Gm,           // General MIDI / GS - what every other output expects
    Mt32,         // Roland MT-32 / CM-32L, including jmp's built-in one
};

struct Row {
    int      slot = -1;         // index into the song's instrument table
    QString  oplName;           // the name the song gives the slot
    long     notes = 0;         // note events this slot actually plays
    Origin   origin = Origin::Unmatched;

    bool     drum = false;      // route to MIDI channel 10
    int      program = 0;       // GM program, 0-based
    int      bankMsb = 0;       // GS variation bank (CC0); 0 is plain GM
    int      drumNote = 38;     // GM percussion note when drum

    // Every MIDI channel this slot is played on. Kept so a change made while
    // the song is sounding can be sent straight out as a program change rather
    // than waiting for the stream to be rebuilt.
    QVector<int> channels;

    int      mt32Tone = 0;      // 1..128 when the song file carried one, else 0
    bool     approximate = false;  // the MT-32 tone has no exact GM equivalent

    // The MT-32 tone this slot plays when the song targets an MT-32, 1..128.
    // Independent of `program` so the two sets of choices coexist; defaults to
    // the song's own tone where it had one, otherwise to whatever the GM match
    // inverts to (mt32map::fromGeneralMidi), otherwise tone 1 - which is where
    // GAYOBANG started every slot.
    int      mt32Program = 1;

    // What this row started as, so the dialog can offer "revert" and can tell
    // an edit apart from a default.
    Origin   baseOrigin = Origin::Unmatched;
    bool     baseDrum = false;
    int      baseProgram = 0;
    int      baseBankMsb = 0;
    int      baseDrumNote = 38;
    int      baseMt32Program = 1;

    bool edited() const {
        return drum != baseDrum || program != baseProgram ||
               bankMsb != baseBankMsb || drumNote != baseDrumNote ||
               mt32Program != baseMt32Program;
    }
};

// .gyb and .oka - the two that play through the OPL engine today. .okm and
// .okw already play as MIDI and are deliberately left alone.
bool isSupported(const QString& path);

// The song's slots with a default assignment for each: the file's own MT-32
// tone where it has one, otherwise a name match, otherwise nothing. Any sidecar
// is applied on top. Empty if the file cannot be read.
QVector<Row> buildPlan(const QString& path);

// The song as a standard MIDI file with `plan` applied - percussion moved to
// channel 10, note offs synthesised, a GS reset in front. Empty on failure,
// with the reason in `error` when it is not null.
QByteArray toMidi(const QString& path, const QVector<Row>& plan,
                  QString* error = nullptr, Target target = Target::Gm);

// Sidecar: `<song>.<ext>.ini`, beside the song. The full extension is kept
// because the library holds BEYOND.GYB next to BEYOND.OKA - same name, two
// different instrument tables. Written only when something was edited; read
// falls back to the settings folder for songs on read-only media.
QString  sidecarPath(const QString& songPath);
bool     loadSidecar(const QString& songPath, QVector<Row>& plan);
bool     saveSidecar(const QString& songPath, const QVector<Row>& plan);

// Whether this song should play through MIDI rather than OPL. Persisted per
// song in the same sidecar, so the choice survives a restart.
bool midiModeEnabled(const QString& songPath);
void setMidiModeEnabled(const QString& songPath, bool on);

// Which instrument set this song's assignments are written for. Persisted in
// the same sidecar; GM unless the user has said otherwise.
Target targetModule(const QString& songPath);
void   setTargetModule(const QString& songPath, Target target);

// For the dialog's display.
QString originLabel(Origin o);
QString assignmentLabel(const Row& r);

}  // namespace gybokamidi
