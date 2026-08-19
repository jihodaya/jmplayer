#include "gybokamidi.h"

#include "mt32map.h"

#include "convert/gyb.h"
#include "convert/oksori.h"
#include "convert/smf.h"
#include "convert/gmmap.h"
#include "convert/song.h"

#include "settingsmanager.h"

#include <QCryptographicHash>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <map>

namespace gybokamidi {
namespace {

using jmpconv::Bytes;

bool isGyb(const QString& p) { return p.endsWith(".gyb", Qt::CaseInsensitive); }

bool isOksori(const QString& p)
{
    return p.endsWith(".oka", Qt::CaseInsensitive) ||
           p.endsWith(".okm", Qt::CaseInsensitive) ||
           p.endsWith(".okw", Qt::CaseInsensitive);
}

// Read the song once: the SMF its notes make, and the instrument table its
// program changes index. Both engines end at the same two values, which is why
// everything below can be written once rather than twice.
bool readSong(const QString& path, Bytes& smf,
              std::vector<jmpconv::Instrument>& instTable, QString* error)
{
    // Read the bytes here rather than handing jmpconv a path.
    //
    // Its readFile() uses std::ifstream, and on Windows a narrow path goes
    // through the ANSI codepage - so a UTF-8 path with Korean in it does not
    // open. Measured with the new loadall tool on its first run: of 344 .GYB
    // files in the library, the 47 on ASCII paths converted and the 297 under
    // Korean folder names all failed with "Not a readable GYB file", an exact
    // 1:1 match with which paths contain non-ASCII. MIDI mode was broken for
    // most of the library and nobody had noticed, because the songs tried by
    // hand happened to live in D:\mt32>.
    //
    // QFile has no such problem, and the byte-level entry points (gybRead,
    // okaRead) take the data directly, so nothing in convert/ has to change.
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Could not open the song file.");
        return false;
    }
    const QByteArray fileBytes = in.readAll();
    in.close();
    if (fileBytes.isEmpty()) {
        if (error) *error = QObject::tr("The song file is empty.");
        return false;
    }
    const Bytes raw(reinterpret_cast<const uint8_t*>(fileBytes.constData()),
                    reinterpret_cast<const uint8_t*>(fileBytes.constData()) +
                        fileBytes.size());

    if (isGyb(path)) {
        jmpconv::GybFile g;
        if (!jmpconv::gybRead(raw, g)) {
            if (error) *error = QObject::tr("Not a readable GYB file.");
            return false;
        }
        // baseBpmOverride 0 keeps the file's own base tempo (GYB 0x34).
        jmpconv::Song s = jmpconv::gybToSong(g, 0.0);
        smf = jmpconv::smfBuild(s.division, s.tempo, s.tracks);
        instTable = g.instruments;
        return !smf.empty();
    }

    if (isOksori(path)) {
        jmpconv::OkaFile f;
        if (!jmpconv::okaRead(raw, f)) {
            if (error) *error = QObject::tr("Not a readable Oksori Music File.");
            return false;
        }
        // The Oksori container carries a standard SMF; nothing has to be built.
        smf = f.midi;
        instTable = f.instruments;
        return !smf.empty();
    }

    if (error) *error = QObject::tr("Not a .GYB or Oksori file.");
    return false;
}

// Percussion names jmpconv's rules miss, applied on top of them.
//
// This lives here rather than in convert/gmmap.cpp on purpose: that file is
// jmpconv's, kept byte-identical so its corpus check still vouches for it and
// so a later copy from okpiri does not collide. These are jmp's own additions
// and can be promoted upstream whenever okpiri is worked on again.
//
// Measured over the 4,893 slots in this library: 32 names, 92 uses, were
// classified as anything but percussion - `sc-cym`, `synsnr`, `rimshot`,
// `clapping`, `sdrum`, `brush`, `dmbongo`, `sss-bd`, plain `drum`. Locking a
// row's type instead of fixing this would have left every one of them playing
// a snare as a piano. Unrecognised slots drop from 533 to 470.
//
// Substring matching is blunt, so it only runs where the base rules found
// nothing - never overriding a confident melodic match - with two deliberate
// exceptions, `sc-cym` and `synsnr`, which the `sc-` and `syn` melodic
// prefixes claim first and which are always cymbals and snares. `marimb` is
// excluded outright: "marimba" contains "rim" and is a real melodic
// instrument (GM 12).
struct DrumHint { const char* key; int note; };
const DrumHint kDrumHints[] = {
    { "rimshot", 37 },  // side stick
    { "crash",   49 },  { "cymbl", 49 },  { "cym", 49 },
    { "snr",     38 },  { "sndrum", 38 }, { "sdrum", 38 }, { "brush", 38 },
    { "clap",    39 },
    { "bongo",   60 },
    { "hihat",   42 },  { "hh", 42 },
    { "basdrum", 36 },  { "drum", 36 },   { "bd", 36 },
};

QString normalisedName(const QString& raw)
{
    QString t;
    for (QChar c : raw)
        if (c.isLetterOrNumber() || c == '-') t.append(c.toLower());
    while (t.size() > 1 && t.back().isDigit()) t.chop(1);
    return t;
}

// -1 when nothing applies.
int supplementalDrumNote(const QString& oplName, bool baseMatched, bool baseDrum)
{
    const QString n = normalisedName(oplName);
    if (n.isEmpty() || n.contains("marimb")) return -1;
    if (baseDrum) return -1;
    if (baseMatched && !(n.startsWith("sc-cym") || n.startsWith("synsnr")))
        return -1;
    for (const DrumHint& h : kDrumHints)
        if (n.contains(QLatin1String(h.key))) return h.note;
    return -1;
}

// One row per slot. planPatches() works per (track, slot) because a slot can be
// played on more than one track, but the instrument list the DOS dialog showed -
// and the one worth editing - is per slot, so the tracks are folded together and
// their note counts summed.
QVector<Row> rowsFromPlan(const std::vector<jmpconv::PatchAssignment>& plan,
                          const std::vector<jmpconv::Instrument>& instTable)
{
    QVector<Row> rows;
    std::map<int, int> seen;   // slot -> index in rows

    for (const jmpconv::PatchAssignment& a : plan) {
        if (a.slot < 0) continue;

        auto it = seen.find(a.slot);
        if (it != seen.end()) {
            Row& prev = rows[it->second];
            prev.notes += a.notes;
            if (!prev.channels.contains(a.channel)) prev.channels.append(a.channel);
            continue;
        }

        Row r;
        r.slot = a.slot;
        r.channels.append(a.channel);
        r.oplName = QString::fromLatin1(a.name.c_str()).trimmed();
        r.notes = a.notes;

        const unsigned char flag =
            (a.slot < int(instTable.size())) ? instTable[a.slot].flag : 0;

        if (mt32map::flagCarriesAssignment(flag)) {
            // Somebody sat in front of the DOS program and picked this one.
            // Nothing we compute beats that, so it wins over the name match.
            const int tone = mt32map::toneOfFlag(flag);
            const mt32map::GmChoice gm = mt32map::toGeneralMidi(tone);
            r.origin      = Origin::SongFile;
            r.mt32Tone    = tone;
            r.approximate = !gm.exact;
            r.drum        = gm.drum;
            r.program     = gm.program;
            r.drumNote    = gm.drumNote;
            r.bankMsb     = 0;
        } else if (a.autoMatched) {
            r.origin   = Origin::NameMatch;
            r.drum     = a.drum;
            r.program  = a.program;
            r.bankMsb  = a.bankMsb;
            r.drumNote = a.drumNote;
        } else {
            r.origin = Origin::Unmatched;
        }

        if (r.origin != Origin::SongFile) {
            const int note = supplementalDrumNote(r.oplName, a.autoMatched, r.drum);
            if (note >= 0) {
                r.origin   = Origin::NameMatch;
                r.drum     = true;
                r.drumNote = note;
                r.bankMsb  = 0;
            }
        }

        r.baseOrigin   = r.origin;
        r.baseDrum     = r.drum;
        r.baseProgram  = r.program;
        r.baseBankMsb  = r.bankMsb;
        r.baseDrumNote = r.drumNote;

        seen[a.slot] = rows.size();
        rows.append(r);
    }
    return rows;
}

QString fallbackSidecarPath(const QString& songPath)
{
    // Songs can live on a CD or a write-protected share. Keep those in the
    // settings folder, keyed by a hash so two songs of the same name in
    // different folders do not collide.
    const QString dir = SettingsManager::instance().storageDir() + "/patch";
    QDir().mkpath(dir);
    const QByteArray h = QCryptographicHash::hash(
        QFileInfo(songPath).absoluteFilePath().toLower().toUtf8(),
        QCryptographicHash::Md5).toHex().left(12);
    return dir + "/" + QFileInfo(songPath).fileName() + "." +
           QString::fromLatin1(h) + ".ini";
}

QString existingSidecar(const QString& songPath)
{
    const QString beside = sidecarPath(songPath);
    if (QFile::exists(beside)) return beside;
    const QString fb = fallbackSidecarPath(songPath);
    if (QFile::exists(fb)) return fb;
    return QString();
}

}  // namespace

bool isSupported(const QString& path)
{
    // .gyb and .oka only. .okm and .okw already play through the ordinary MIDI
    // path and have done for a long time; routing them through here would
    // silently change how they sound, which is not what this feature is for.
    return isGyb(path) || path.endsWith(".oka", Qt::CaseInsensitive);
}

QVector<Row> buildPlan(const QString& path)
{
    Bytes smf;
    std::vector<jmpconv::Instrument> instTable;
    if (!readSong(path, smf, instTable, nullptr)) return {};

    QVector<Row> rows = rowsFromPlan(jmpconv::planPatches(smf, instTable), instTable);
    loadSidecar(path, rows);
    return rows;
}

QByteArray toMidi(const QString& path, const QVector<Row>& plan, QString* error)
{
    Bytes smf;
    std::vector<jmpconv::Instrument> instTable;
    if (!readSong(path, smf, instTable, error)) return {};

    // Regenerate the per-(track, slot) plan so the track and channel numbers are
    // right, then overwrite each entry from the caller's per-slot decision.
    std::vector<jmpconv::PatchAssignment> full = jmpconv::planPatches(smf, instTable);

    std::map<int, const Row*> bySlot;
    for (const Row& r : plan) bySlot[r.slot] = &r;

    for (jmpconv::PatchAssignment& a : full) {
        auto it = bySlot.find(a.slot);
        if (it == bySlot.end()) continue;
        const Row& r = *it->second;
        a.drum     = r.drum;
        a.program  = r.program;
        a.bankMsb  = r.bankMsb;
        a.drumNote = r.drumNote;
    }

    // Three things are wrong with the stream as it stands and this fixes all of
    // them: program changes index the song's OPL table rather than GM,
    // percussion sits on ordinary channels, and there are almost no note offs
    // because an OPL voice is monophonic. See gmmap.h.
    const Bytes out = jmpconv::smfToGeneralMidi(smf, full, true, nullptr);
    if (out.empty()) {
        if (error) *error = QObject::tr("Could not build a MIDI stream.");
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(out.data()), int(out.size()));
}

QString sidecarPath(const QString& songPath)
{
    // Keep the whole extension: BEYOND.GYB and BEYOND.OKA sit in one folder and
    // have different instrument tables, so BEYOND.ini would be one file for two
    // songs.
    return songPath + ".ini";
}

bool loadSidecar(const QString& songPath, QVector<Row>& plan)
{
    const QString file = existingSidecar(songPath);
    if (file.isEmpty()) return false;

    QSettings ini(file, QSettings::IniFormat);
    bool any = false;

    for (Row& r : plan) {
        const QString g = QString("slot%1").arg(r.slot);
        ini.beginGroup(g);
        const QString name = ini.value("name").toString();
        // A song edited since the sidecar was written can have different slots.
        // Trust the name over the index and leave the row at its default if they
        // disagree, rather than silently applying somebody else's choice.
        if (!name.isEmpty() && name.compare(r.oplName, Qt::CaseInsensitive) == 0) {
            // Clamped because this file is meant to be readable and editable
            // by hand. An out-of-range program reaches the stream as a data
            // byte with its top bit set, which is a malformed message rather
            // than a wrong instrument. Same lesson as Sc55/AudioBuffer: a value
            // a person can type has to be checked, not trusted.
            r.drum     = ini.value("drum", r.drum).toBool();
            r.program  = qBound(0, ini.value("program",  r.program).toInt(),  127);
            r.bankMsb  = qBound(0, ini.value("bank",     r.bankMsb).toInt(),  127);
            r.drumNote = qBound(0, ini.value("note",     r.drumNote).toInt(), 127);
            r.origin   = Origin::User;
            any = true;
        }
        ini.endGroup();
    }
    return any;
}

bool saveSidecar(const QString& songPath, const QVector<Row>& plan)
{
    QString file = sidecarPath(songPath);

    // Probe rather than assume: a song can sit on a CD or a share we may not
    // write to, and failing silently would lose the user's work.
    {
        QFile probe(file);
        if (!probe.open(QIODevice::Append)) {
            file = fallbackSidecarPath(songPath);
        } else {
            probe.close();
            if (probe.size() == 0) probe.remove();
        }
    }

    QSettings ini(file, QSettings::IniFormat);
    ini.setValue("jmp/version", 1);
    ini.setValue("jmp/source", QFileInfo(songPath).fileName());
    // Written here rather than when the button is pressed, so the file appears
    // once, on purpose.
    ini.setValue("jmp/midiMode", midiModeEnabled(songPath));

    for (const Row& r : plan) {
        const QString g = QString("slot%1").arg(r.slot);
        ini.beginGroup(g);
        if (r.edited()) {
            ini.setValue("name", r.oplName);
            ini.setValue("drum", r.drum);
            ini.setValue("program", r.program);
            ini.setValue("bank", r.bankMsb);
            ini.setValue("note", r.drumNote);
        } else {
            // Reverted to its default: drop the group so the file stays a record
            // of decisions actually made, not of every slot in the song.
            ini.remove("");
        }
        ini.endGroup();
    }
    ini.sync();
    return ini.status() == QSettings::NoError;
}

// Pressing the MIDI button must not leave a file beside the song. The choice
// lives in memory for the session and is only written when the user presses
// Save in the patch dialog, which is also where the instrument assignment is
// written - one deliberate act, one file.
QHash<QString, bool>& sessionMidiMode()
{
    static QHash<QString, bool> map;
    return map;
}

bool midiModeEnabled(const QString& songPath)
{
    const QString key = QFileInfo(songPath).absoluteFilePath().toLower();
    auto it = sessionMidiMode().constFind(key);
    if (it != sessionMidiMode().constEnd()) return it.value();

    const QString file = existingSidecar(songPath);
    if (file.isEmpty()) return false;
    QSettings ini(file, QSettings::IniFormat);
    return ini.value("jmp/midiMode", false).toBool();
}

void setMidiModeEnabled(const QString& songPath, bool on)
{
    sessionMidiMode().insert(QFileInfo(songPath).absoluteFilePath().toLower(), on);
}

QString originLabel(Origin o)
{
    switch (o) {
        case Origin::SongFile:  return QObject::tr("from song file");
        case Origin::NameMatch: return QObject::tr("matched by name");
        case Origin::Unmatched: return QObject::tr("not recognised");
        case Origin::User:      return QObject::tr("your choice");
    }
    return QString();
}

QString assignmentLabel(const Row& r)
{
    if (r.origin == Origin::Unmatched && !r.edited()) return QStringLiteral("-");
    if (r.drum) {
        return QString("%1 (%2)")
            .arg(QString::fromLatin1(jmpconv::gmDrumName(r.drumNote)))
            .arg(r.drumNote);
    }
    QString s = QString::fromLatin1(jmpconv::gmProgramName(r.program));
    if (r.bankMsb) s += QString(" [GS %1]").arg(r.bankMsb);
    return s;
}

}  // namespace gybokamidi
