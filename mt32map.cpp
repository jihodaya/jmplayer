#include "mt32map.h"

namespace mt32map {
namespace {

// Extracted from GAYOBANG.EXE at file offset 0x3924b. Entries are a 3-digit
// number followed by the name; the stride alternates between 14 and 15 bytes,
// which is what makes a fixed-stride read drift after entry 61. Spellings are
// the original's, typos included (`PipeOrg3`, `Square0Wave`, `Shakuachi`,
// `Orche  Hit`) so a name shown here matches what the DOS screen showed.
const char* kTones[128] = {
    "Acou Piano1", "Acou Piano2", "Acou Piano3", "Elec Piano1",
    "Elec Piano2", "Elec Piano3", "Elec Piano4", "Honkytonk",
    "Elec Org1",   "Elec Org2",   "Elec Org3",   "Elec Org4",
    "Pipe Org1",   "Pipe Org2",   "PipeOrg3",    "Accordion",
    "Harpsi1",     "Harpsi2",     "Harpsi3",     "Clavi1",
    "Clavi2",      "Clavi3",      "Celesta1",    "Celesta2",
    "Syn Brass1",  "Syn Brass2",  "Syn Brass3",  "Syn Brass4",
    "Syn Bass1",   "Syn Bass2",   "Syn Bass3",   "Syn Bass4",
    "Fantasy",     "Harmo Pan",   "Chorale",     "Glasses",
    "Soundtrack",  "Atmosphere",  "Warm Bell",   "Funny Vox",
    "Echo Bell",   "Ice Rain",    "Oboe 2001",   "Echo Pan",
    "Doctor Solo", "Schooldaze",  "Bellsinger",  "Square0Wave",
    "Str Sect1",   "Str Sect2",   "StrSect3",    "Pizzicato",
    "violin1",     "violin2",     "Cello1",      "Cello2",
    "Contrabass",  "Harp1",       "Harp2",       "Guitar1",
    "Guitar2",     "Elec Gtr1",   "Elec Gtr2",   "Sitar",
    "Acou Bass1",  "Acou Bass2",  "Elec Bass1",  "Elec Bass2",
    "Slap Bass1",  "Slap Bass2",  "Fretless1",   "Fretless2",
    "Flute1",      "Flute2",      "Piccolo1",    "Piccolo2",
    "Recorder",    "Pan Pipes",   "Sax1",        "Sax2",
    "Sax3",        "Sax4",        "Clarinet1",   "Clarinet2",
    "Oboe",        "Engl Horn",   "Bassoon",     "Harmonica",
    "Trumpet1",    "Trumpet2",    "Trombone1",   "Trombone2",
    "Fr Horn1",    "Fr Horn2",    "Tuba",        "Brs Sect1",
    "Brs Sect2",   "Vibe1",       "Vibe2",       "Syn Mallet",
    "Windbell",    "Glock",       "Tube Bell",   "Xylophone",
    "Marimba",     "Koto",        "Sho",         "Shakuachi",
    "Whistle1",    "Whistle2",    "Bottleblow",  "Breathpipe",
    "Timpani",     "Melodic Tom", "Deep Snare",  "Elec Perc1",
    "Elec Perc2",  "Taiko",       "Taiko Rim",   "Cymbal",
    "Castanets",   "Triangle",    "Orche  Hit",  "Telephone",
    "Bird Tweet",  "One Note",    "Water Bells", "Jungle Tune",
};

// MT-32 tone -> General MIDI. Programs are 0-based.
//
// `D` marks a tone that is percussion on a GM module and so belongs on channel
// 10 with a note rather than a program. `~` marks an equivalent that is a
// judgement call rather than a straight correspondence: the MT-32's LA voices
// in the 33-48 range are synth textures GM has no exact name for, and a handful
// of tones (Sho, Castanets, the two phrase tones) have no GM voice at all. Those
// are exactly the rows worth checking by ear in the F5 dialog - `exact` is false
// for them so the dialog can say so.
struct Row { signed char drumNote; short program; bool exact; };
#define M(p)     { -1, (p), true  }     // melodic, exact
#define A(p)     { -1, (p), false }     // melodic, approximate
#define D(n)     { (n), 0,   true  }    // percussion, exact
#define DA(n)    { (n), 0,   false }    // percussion, approximate

const Row kToGm[128] = {
    /*  1 Acou Piano1 */ M(0),   /*  2 Acou Piano2 */ M(0),
    /*  3 Acou Piano3 */ M(1),   /*  4 Elec Piano1 */ M(4),
    /*  5 Elec Piano2 */ M(4),   /*  6 Elec Piano3 */ M(5),
    /*  7 Elec Piano4 */ M(5),   /*  8 Honkytonk   */ M(3),
    /*  9 Elec Org1   */ M(16),  /* 10 Elec Org2   */ M(17),
    /* 11 Elec Org3   */ M(18),  /* 12 Elec Org4   */ M(16),
    /* 13 Pipe Org1   */ M(19),  /* 14 Pipe Org2   */ M(19),
    /* 15 PipeOrg3    */ M(19),  /* 16 Accordion   */ M(21),
    /* 17 Harpsi1     */ M(6),   /* 18 Harpsi2     */ M(6),
    /* 19 Harpsi3     */ M(6),   /* 20 Clavi1      */ M(7),
    /* 21 Clavi2      */ M(7),   /* 22 Clavi3      */ M(7),
    /* 23 Celesta1    */ M(8),   /* 24 Celesta2    */ M(8),
    /* 25 Syn Brass1  */ M(62),  /* 26 Syn Brass2  */ M(63),
    /* 27 Syn Brass3  */ M(62),  /* 28 Syn Brass4  */ M(63),
    /* 29 Syn Bass1   */ M(38),  /* 30 Syn Bass2   */ M(39),
    /* 31 Syn Bass3   */ M(38),  /* 32 Syn Bass4   */ M(39),
    /* 33 Fantasy     */ A(88),  /* 34 Harmo Pan   */ A(89),
    /* 35 Chorale     */ A(52),  /* 36 Glasses     */ A(98),
    /* 37 Soundtrack  */ A(97),  /* 38 Atmosphere  */ A(99),
    /* 39 Warm Bell   */ A(98),  /* 40 Funny Vox   */ A(85),
    /* 41 Echo Bell   */ A(14),  /* 42 Ice Rain    */ A(96),
    /* 43 Oboe 2001   */ A(68),  /* 44 Echo Pan    */ A(102),
    /* 45 Doctor Solo */ A(87),  /* 46 Schooldaze  */ A(98),
    /* 47 Bellsinger  */ A(9),   /* 48 Square0Wave */ M(80),
    /* 49 Str Sect1   */ M(48),  /* 50 Str Sect2   */ M(49),
    /* 51 StrSect3    */ M(48),  /* 52 Pizzicato   */ M(45),
    /* 53 violin1     */ M(40),  /* 54 violin2     */ M(40),
    /* 55 Cello1      */ M(42),  /* 56 Cello2      */ M(42),
    /* 57 Contrabass  */ M(43),  /* 58 Harp1       */ M(46),
    /* 59 Harp2       */ M(46),  /* 60 Guitar1     */ M(24),
    /* 61 Guitar2     */ M(25),  /* 62 Elec Gtr1   */ M(27),
    /* 63 Elec Gtr2   */ M(27),  /* 64 Sitar       */ M(104),
    /* 65 Acou Bass1  */ M(32),  /* 66 Acou Bass2  */ M(32),
    /* 67 Elec Bass1  */ M(33),  /* 68 Elec Bass2  */ M(34),
    /* 69 Slap Bass1  */ M(36),  /* 70 Slap Bass2  */ M(37),
    /* 71 Fretless1   */ M(35),  /* 72 Fretless2   */ M(35),
    /* 73 Flute1      */ M(73),  /* 74 Flute2      */ M(73),
    /* 75 Piccolo1    */ M(72),  /* 76 Piccolo2    */ M(72),
    /* 77 Recorder    */ M(74),  /* 78 Pan Pipes   */ M(75),
    /* 79 Sax1        */ M(65),  /* 80 Sax2        */ M(66),
    /* 81 Sax3        */ M(65),  /* 82 Sax4        */ M(67),
    /* 83 Clarinet1   */ M(71),  /* 84 Clarinet2   */ M(71),
    /* 85 Oboe        */ M(68),  /* 86 Engl Horn   */ M(69),
    /* 87 Bassoon     */ M(70),  /* 88 Harmonica   */ M(22),
    /* 89 Trumpet1    */ M(56),  /* 90 Trumpet2    */ M(56),
    /* 91 Trombone1   */ M(57),  /* 92 Trombone2   */ M(57),
    /* 93 Fr Horn1    */ M(60),  /* 94 Fr Horn2    */ M(60),
    /* 95 Tuba        */ M(58),  /* 96 Brs Sect1   */ M(61),
    /* 97 Brs Sect2   */ M(61),  /* 98 Vibe1       */ M(11),
    /* 99 Vibe2       */ M(11),  /*100 Syn Mallet  */ A(12),
    /*101 Windbell    */ A(98),  /*102 Glock       */ M(9),
    /*103 Tube Bell   */ M(14),  /*104 Xylophone   */ M(13),
    /*105 Marimba     */ M(12),  /*106 Koto        */ M(107),
    /*107 Sho         */ A(111), /*108 Shakuachi   */ M(77),
    /*109 Whistle1    */ M(78),  /*110 Whistle2    */ M(78),
    /*111 Bottleblow  */ M(76),  /*112 Breathpipe  */ A(121),
    /*113 Timpani     */ M(47),  /*114 Melodic Tom */ M(117),
    /*115 Deep Snare  */ D(38),  /*116 Elec Perc1  */ M(118),
    /*117 Elec Perc2  */ M(118), /*118 Taiko       */ M(116),
    /*119 Taiko Rim   */ DA(37), /*120 Cymbal      */ D(49),
    /*121 Castanets   */ DA(75), /*122 Triangle    */ D(81),
    /*123 Orche  Hit  */ M(55),  /*124 Telephone   */ M(124),
    /*125 Bird Tweet  */ M(123), /*126 One Note    */ A(55),
    /*127 Water Bells */ A(98),  /*128 Jungle Tune */ A(55),
};

#undef M
#undef A
#undef D
#undef DA

}  // namespace

const char* toneName(int tone)
{
    if (tone < 1 || tone > 128) return "";
    return kTones[tone - 1];
}

GmChoice toGeneralMidi(int tone)
{
    GmChoice out;
    if (tone < 1 || tone > 128) return out;
    const Row& r = kToGm[tone - 1];
    out.exact = r.exact;
    if (r.drumNote >= 0) {
        out.drum = true;
        out.drumNote = r.drumNote;
    } else {
        out.program = r.program;
    }
    return out;
}

int fromGeneralMidi(int gmProgram)
{
    if (gmProgram < 0 || gmProgram > 127) return 0;

    // First match wins, and the table is in tone order, so a GM program that
    // several tones map to resolves to the lowest-numbered one - which for the
    // MT-32 is the plainest of the set (Acou Piano1 before Piano2, Elec Org1
    // before Org2). That is the right default to hand someone who has not
    // chosen yet.
    //
    // Exact equivalents are preferred over the ones flagged as judgement calls,
    // so the scan runs twice rather than taking whatever comes first.
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantExact = (pass == 0);
        for (int i = 0; i < 128; ++i) {
            const Row& r = kToGm[i];
            if (r.drumNote >= 0) continue;          // percussion, not a melodic tone
            if (r.exact != wantExact) continue;
            if (r.program == gmProgram) return i + 1;
        }
    }
    return 0;
}

}  // namespace mt32map
