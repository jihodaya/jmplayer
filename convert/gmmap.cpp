#include "gmmap.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "smf.h"

namespace jmpconv {

namespace {

struct Rule {
    const char* key;
    uint8_t value;      // GM program, or GM drum note
    uint8_t bank;       // GS variation bank (CC0); 0 = plain GM
};

// Percussion first, longest keys first inside each group, so "cymcrash" wins
// over "cym" and "rksnare" over "snare". Values are GM drum notes.
const Rule kDrums[] = {
    {"clhihat", 42, 0}, {"ophihat", 46, 0}, {"openhh", 46, 0}, {"hihat", 42, 0},
    {"cymcrash", 49, 0}, {"itscym", 51, 0}, {"cymbal", 49, 0}, {"ride", 51, 0},
    {"dbkick", 36, 0}, {"bdrum", 36, 0}, {"kick", 36, 0},
    {"rksnare", 38, 0}, {"snaresy", 40, 0}, {"snaresr", 40, 0}, {"snare", 38, 0},
    {"brushes", 40, 0}, {"tomtom", 45, 0}, {"tom", 45, 0},
    {"sc-drum", 38, 0}, {"shock", 39, 0}, {"clave", 75, 0}, {"conga", 64, 0},
    {"timbal", 65, 0}, {"cowbell", 56, 0}, {"tamb", 54, 0}, {"triangl", 81, 0},
    {"logdrum", 76, 0}, {"tincan", 80, 0}, {"scratch", 58, 0},
};

// GM programs, with a GS variation bank where GS has a closer voice than the
// plain GM one -- the FM originals lean bright and synthetic, which is exactly
// what the GS variations are.
const Rule kMelodic[] = {
    {"phgpiano", 0, 0},  {"elpiano", 4, 0},   {"honkyton", 3, 0},
    {"piano", 0, 0},     {"harpsi", 6, 0},    {"clavi", 7, 0},
    {"celesta", 8, 0},   {"glocken", 9, 0},   {"marimba", 12, 0},
    {"xylo", 13, 0},     {"vibra", 11, 0},
    {"bells", 14, 0},    {"tubular", 14, 0},  {"handbell", 14, 8},
    {"organas", 16, 0},  {"pip-org", 19, 0},  {"pipeorg", 19, 0},
    {"el-org", 17, 0},   {"elorgan", 17, 0},  {"organ", 16, 0},
    {"t-accor", 21, 0},  {"accordn", 21, 0},  {"accord", 21, 0},
    {"harmonica", 22, 0},{"harmopan", 22, 0},
    {"guitarac", 25, 0}, {"acguit", 25, 0},
    {"guithv", 30, 8},   {"hvymetl", 30, 8},  {"guitdis", 30, 0},
    {"odrive", 29, 0},   {"gtrelc", 27, 0},   {"t-guit", 27, 0},
    {"elguit", 27, 0},   {"elg", 27, 0},      {"eg", 27, 0},
    {"guitar", 24, 0},   {"sitar", 104, 0},   {"banjo", 105, 0},
    {"sc-elb", 33, 0},   {"elbass", 33, 0},   {"acbass", 32, 0},
    {"synbass", 38, 0},  {"popbass", 33, 0},  {"longbass", 33, 0},
    {"beachbas", 33, 0}, {"ykabass", 33, 0},  {"elb", 33, 0},
    {"bass", 33, 0},
    {"elviolin", 40, 0}, {"sss-vln", 40, 0},  {"violin", 40, 0},
    {"vio", 40, 0},      {"viola", 41, 0},    {"cello", 42, 0},
    {"contrab", 43, 0},
    {"fstrp", 48, 8},    {"strnlong", 48, 8}, {"string", 48, 0},
    {"harp", 46, 0},
    {"sftbrss", 61, 8},  {"abress", 61, 0},   {"abrss", 61, 0},
    {"synbras", 62, 8},  {"brass", 61, 0},    {"trumpet", 56, 0},
    {"tromb", 57, 0},    {"tuba", 58, 0},     {"rwhorn", 60, 0},
    {"horn", 60, 0},     {"tr", 56, 0},
    {"saxten", 66, 0},   {"aalto", 65, 0},    {"sax", 65, 0},
    {"bt-oboe", 68, 0},  {"oboev", 68, 0},    {"oboe", 68, 0},
    {"bassoon", 70, 0},  {"clarinet", 71, 0},
    {"sss-flut", 73, 0}, {"snakefl", 73, 0},  {"sftfl", 73, 0},
    {"flute", 73, 0},    {"piccolo", 72, 0},  {"recorder", 74, 0},
    {"whistle", 78, 0},  {"wood", 74, 0},
    {"newlead", 81, 8},  {"a-lead", 80, 8},   {"moogsynt", 81, 8},
    {"lead", 80, 0},     {"elclav", 7, 0},    {"bbr-pian", 0, 0},
    {"fantasy", 88, 0},  {"chorale", 52, 0},  {"choir", 52, 0},
    {"voice", 53, 0},    {"glasses", 98, 0},  {"sndtrack", 97, 0},
    {"keybrd", 4, 0},    {"real", 4, 0},      {"syn", 80, 8},
    {"sc-", 80, 0},      {"phone", 124, 0},   {"silience", 0, 0},
};

const char* kGmNames[128] = {
    "Acoustic Grand", "Bright Piano", "Electric Grand", "Honky-tonk",
    "Rhodes Piano", "Chorused Piano", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Perc Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
    "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
    "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead",
    "Charang Lead", "Voice Lead", "Fifths Lead", "Bass+Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad",
    "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX",
    "Brightness FX", "Goblins FX", "Echoes FX", "Sci-Fi FX",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

struct DrumName { int note; const char* name; };
const DrumName kDrumNames[] = {
    {35, "Acoustic Bass Drum"}, {36, "Bass Drum 1"}, {37, "Side Stick"},
    {38, "Acoustic Snare"},     {39, "Hand Clap"},   {40, "Electric Snare"},
    {41, "Low Floor Tom"},      {42, "Closed Hi-Hat"}, {43, "High Floor Tom"},
    {44, "Pedal Hi-Hat"},       {45, "Low Tom"},     {46, "Open Hi-Hat"},
    {47, "Low-Mid Tom"},        {48, "Hi-Mid Tom"},  {49, "Crash Cymbal 1"},
    {50, "High Tom"},           {51, "Ride Cymbal 1"}, {52, "Chinese Cymbal"},
    {53, "Ride Bell"},          {54, "Tambourine"},  {55, "Splash Cymbal"},
    {56, "Cowbell"},            {57, "Crash Cymbal 2"}, {58, "Vibraslap"},
    {59, "Ride Cymbal 2"},      {64, "Low Conga"},   {65, "High Timbale"},
    {75, "Claves"},             {76, "Hi Wood Block"}, {80, "Mute Triangle"},
    {81, "Open Triangle"},
};

std::string normalise(const std::string& s) {
    std::string t;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || c == '-') t.push_back(char(std::tolower(u)));
    }
    // Bank names pad with digits: piano001, abrss000, sn5. Drop trailing ones,
    // but keep the name itself non-empty.
    while (t.size() > 1 && std::isdigit((unsigned char)t.back())) t.pop_back();
    return t;
}

bool startsWith(const std::string& s, const char* key) {
    size_t n = std::strlen(key);
    return s.size() >= n && s.compare(0, n, key) == 0;
}

}  // namespace

const char* gmProgramName(int p) {
    return (p >= 0 && p < 128) ? kGmNames[p] : "?";
}

const char* gmDrumName(int note) {
    for (const DrumName& d : kDrumNames)
        if (d.note == note) return d.name;
    return "Percussion";
}

const std::vector<int>& gmDrumNotes() {
    static std::vector<int> notes = [] {
        std::vector<int> v;
        for (const DrumName& d : kDrumNames) v.push_back(d.note);
        return v;
    }();
    return notes;
}

const std::vector<GsBankOption>& gsBankOptions() {
    static std::vector<GsBankOption> banks = {
        {0, "Bank 0 (Standard GM)"},
        {8, "Bank 8 (GS Variation / Detuned)"},
        {16, "Bank 16 (GS Variation)"},
        {24, "Bank 24 (GS Variation)"},
        {32, "Bank 32 (GS Variation)"},
    };
    return banks;
}

PatchAssignment gmForInstrument(const std::string& oplName) {
    PatchAssignment out;
    out.name = oplName;
    const std::string n = normalise(oplName);
    if (n.empty()) return out;

    for (const Rule& r : kDrums) {
        if (n.find(r.key) != std::string::npos) {
            out.drum = true;
            out.drumNote = r.value;
            out.autoMatched = true;
            return out;
        }
    }
    if (n == "sn") {          // the bank's short form for a snare
        out.drum = true; out.drumNote = 38; out.autoMatched = true; return out;
    }
    for (const Rule& r : kMelodic) {
        if (startsWith(n, r.key)) {
            out.program = r.value;
            out.bankMsb = r.bank;
            out.autoMatched = true;
            return out;
        }
    }
    return out;
}

std::vector<PatchAssignment> planPatches(const Bytes& midi,
                                         const std::vector<Instrument>& slots) {
    std::vector<PatchAssignment> plan;
    size_t total = smfLength(midi);
    if (!total || slots.empty()) return plan;

    size_t ntrk = rdBE16(&midi[10]);
    size_t off = 8 + rdBE32(&midi[4]);

    for (size_t t = 0; t < ntrk; ++t) {
        size_t end = off + 8 + rdBE32(&midi[off + 4]);
        size_t p = off + 8;
        uint8_t running = 0;
        int current = -1;
        int channel = -1;
        std::map<int, size_t> rowOf;      // slot -> index into plan

        while (p < end) {
            readVarLen(midi.data(), p);
            uint8_t b = midi[p];
            uint8_t status = (b < 0x80) ? running : b;
            if (b >= 0x80) ++p;
            if (status == 0xFF) { ++p; p += readVarLen(midi.data(), p); continue; }
            if (status == 0xF0 || status == 0xF7) {
                p += readVarLen(midi.data(), p);
                continue;
            }
            running = status;
            uint8_t hi = status & 0xF0;
            if (channel < 0) channel = status & 0x0F;

            if (hi == 0xC0) {
                current = midi[p];
                if (rowOf.find(current) == rowOf.end()) {
                    PatchAssignment a = gmForInstrument(
                        current < int(slots.size()) ? slots[current].name : "");
                    a.track = int(t);
                    a.channel = channel;
                    a.slot = current;
                    rowOf[current] = plan.size();
                    plan.push_back(a);
                }
                p += 1;
            } else if (hi == 0xD0) {
                p += 1;
            } else {
                if (hi == 0x90 && midi[p + 1] && current >= 0) {
                    auto it = rowOf.find(current);
                    if (it != rowOf.end()) plan[it->second].notes++;
                }
                p += 2;
            }
        }
        off = end;
    }
    return plan;
}

Bytes smfToGeneralMidi(const Bytes& midi, const std::vector<PatchAssignment>& plan,
                       bool gsReset, GmStats* stats) {
    size_t total = smfLength(midi);
    if (!total || plan.empty()) return midi;
    GmStats st;

    // (track, slot) -> assignment
    std::map<std::pair<int, int>, const PatchAssignment*> byKey;
    for (const PatchAssignment& a : plan) byKey[{a.track, a.slot}] = &a;

    size_t ntrk = rdBE16(&midi[10]);
    size_t headLen = 8 + rdBE32(&midi[4]);

    Bytes out(midi.begin(), midi.begin() + headLen);
    size_t off = headLen;

    for (size_t t = 0; t < ntrk; ++t) {
        size_t end = off + 8 + rdBE32(&midi[off + 4]);

        // Does this track play percussion? Decided by the plan, so an edit in
        // the dialog moves the whole track.
        bool isDrum = false;
        for (const PatchAssignment& a : plan)
            if (a.track == int(t) && a.drum) isDrum = true;
        if (isDrum) st.drumTracks++;

        Bytes body;
        size_t p = off + 8;
        uint8_t running = 0;
        const PatchAssignment* current = nullptr;
        int sounding = -1;
        uint8_t soundingCh = 0;
        bool movedChannel = false;

        // Call right after the delta bytes have been written: it consumes that
        // delta for the note off and leaves a zero delta for the event that
        // was actually due.
        auto releaseHeldInline = [&]() {
            if (sounding < 0) return;
            body.push_back(uint8_t(0x80 | soundingCh));
            body.push_back(uint8_t(sounding));
            body.push_back(0);
            pushVarLen(body, 0);
            sounding = -1;
            st.notesEnded++;
            running = 0;
        };

        // A GS reset up front, so variation banks are honoured.
        if (gsReset && t == 0) {
            static const uint8_t kGsReset[] = {0x41, 0x10, 0x42, 0x12, 0x40,
                                               0x00, 0x7F, 0x00, 0x41, 0xF7};
            pushVarLen(body, 0);
            body.push_back(0xF0);
            pushVarLen(body, uint32_t(sizeof(kGsReset)));
            body.insert(body.end(), kGsReset, kGsReset + sizeof(kGsReset));
        }

        while (p < end) {
            size_t deltaStart = p;
            readVarLen(midi.data(), p);
            body.insert(body.end(), midi.begin() + deltaStart, midi.begin() + p);

            uint8_t b = midi[p];
            uint8_t status = (b < 0x80) ? running : b;
            if (b >= 0x80) ++p;

            if (status == 0xFF) {
                size_t metaStart = p;
                ++p;
                uint32_t len = readVarLen(midi.data(), p);
                p += len;
                if (midi[metaStart] == 0x2F) releaseHeldInline();
                body.push_back(0xFF);
                body.insert(body.end(), midi.begin() + metaStart, midi.begin() + p);
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                size_t sysStart = p;
                uint32_t len = readVarLen(midi.data(), p);
                p += len;
                body.push_back(status);
                body.insert(body.end(), midi.begin() + sysStart, midi.begin() + p);
                continue;
            }

            uint8_t hi = status & 0xF0;
            uint8_t ch = status & 0x0F;
            if (isDrum) {
                ch = uint8_t(kGmDrumChannel);
            } else if (ch == kGmDrumChannel) {
                ch = kGmMelodicRescue;
                if (!movedChannel) { movedChannel = true; st.channelsMoved++; }
            }

            if (hi == 0x90 && midi[p + 1] != 0) {
                releaseHeldInline();
                uint8_t pitch = (isDrum && current && current->drum)
                                    ? uint8_t(current->drumNote) : midi[p];
                body.push_back(uint8_t(0x90 | ch));
                body.push_back(pitch);
                body.push_back(midi[p + 1] ? midi[p + 1] : 1);
                sounding = pitch;
                soundingCh = ch;
                running = uint8_t(0x90 | ch);
                p += 2;
                continue;
            }
            if (hi == 0x80 || hi == 0x90) {
                uint8_t pitch = (isDrum && current && current->drum)
                                    ? uint8_t(current->drumNote) : midi[p];
                if (sounding == int(pitch)) sounding = -1;
                body.push_back(uint8_t(0x80 | ch));
                body.push_back(pitch);
                body.push_back(0);
                running = uint8_t(0x80 | ch);
                p += 2;
                continue;
            }

            running = status;
            if (hi == 0xC0) {
                auto it = byKey.find({int(t), int(midi[p])});
                current = (it != byKey.end()) ? it->second : nullptr;
                if (current && !isDrum && current->bankMsb) {
                    body.push_back(uint8_t(0xB0 | ch));
                    body.push_back(0x00);
                    body.push_back(uint8_t(current->bankMsb));
                    pushVarLen(body, 0);
                }
                body.push_back(uint8_t(0xC0 | ch));
                body.push_back(isDrum ? 0 : uint8_t(current ? current->program : 0));
                running = uint8_t(0xC0 | ch);
                p += 1;
            } else if (hi == 0xD0) {
                body.push_back(uint8_t(hi | ch));
                body.push_back(midi[p]);
                p += 1;
            } else {
                body.push_back(uint8_t(hi | ch));
                body.push_back(midi[p]);
                body.push_back(midi[p + 1]);
                p += 2;
            }
        }

        out.push_back('M'); out.push_back('T'); out.push_back('r'); out.push_back('k');
        pushBE32(out, uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        off = end;
    }

    if (stats) *stats = st;
    return out;
}

}  // namespace jmpconv
