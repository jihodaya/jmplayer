# `.OKA` `.OKM` `.OKW` — Oksori karaoke (NORE45)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: decompilation of NORE45.EXE, a round trip that rebuilds 86 reference files byte for byte, measurements over 117 `.OKA` songs in the library.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../oka-okm-okw.md)

The song file of the Oksori karaoke program NORE45. Three extensions share
**one container**, which holds **an XOR-masked Standard MIDI File (SMF)**,
lyrics and instruments.

| Extension | How it plays |
|---|---|
| `.OKA` | The SMF is played **through an OPL FM chip**, with the instruments in the song |
| `.OKM` | The SMF is played through a MIDI synthesiser |
| `.OKW` | As `.OKM`, with recorded (vocal) audio appended to the file |

## 1. The container

✅ Rebuilding 86 reference files from the layout below gives **all 86
byte-identical to the originals.** The size fields, the instrument count and
the plaintext copy are all recomputed, so the layout is understood rather than
copied.

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 17 | `"Oksori Music File"` (plaintext signature) |
| `0x01A` | 3 | `1A 0A 10` |
| `0x027` | … | Title and lyric preview (plaintext, Johab Korean). Title at `0x27`, preview at `0x85` |
| `0x15A` | 1 | **Melody (vocal) channel** (counted from 0) |
| `0x1AE` | 4 | Size of the lyric sync block |
| `0x1B2` | 2 | Size of the lyric text block |
| `0x1B4` | 2 | Size of the spare block |
| `0x1CA` | 2 | Total body size (the sum of the five blocks below) |
| `0x1CE` | 4 | **MIDI size** |
| `0x1D2` | 2 | Size of the instrument block |
| `0x1F2` | 1 | `0x01` |
| `0x1F6` | 2 | Recording sample rate (`.OKW` only) |
| `0x1F8` | 1 | Number of instrument records |
| `0x210` | 256 | **A plaintext copy of the first 256 bytes of the decrypted SMF** |
| `0x310` | … | **The body. All of it XOR `0xA8`** |

The body follows in this order, every byte XOR-ed with `0xA8`:

```
0x310   MIDI (SMF)          size = [0x1CE]
        lyric sync block    size = [0x1AE]
        lyric text block    size = [0x1B2]
        spare block         size = [0x1B4]
        instrument records  size = [0x1D2]   (38 bytes × count)
(after) .OKW recording — not XOR-ed
```

XOR it with `0xA8` and it is an ordinary SMF starting with `MThd`.

## 2. Lyrics

✅ **Text block**: Johab Korean, lines separated by `CR LF`.

✅ **Sync block**: a run of 5-byte records.

```
u16  tick (SMF ticks)
u16  byte position in the text block — the character to highlight at this tick
u8   ❓
```

Every syllable has its exact time in a table, so `.OKM` lyric sync needs no
guessing. (`.NOB` has no such table; the shape of its lyric block carries the
timing instead — [nob.md](nob.md).)

## 3. Playing `.OKA` through OPL

NORE45 uses **the same OPL driver** as Gayobang: `FUN_19aa_1162` is
GAYOBANG's `FUN_255a_1222` function for function (rhythm bit, register 8 =
`0x40`, note 24 on channel 8, note 31 on channel 7, 11 channels). So every rule
in section 5 of [gyb.md](gyb.md) applies as is; below are the rules for mapping
the SMF onto the OPL.

| SMF event | Interpretation | |
|---|---|---|
| Channels 0-10 | 1:1 onto OPL channels 0-10. Rhythm layout as in GYB (6 bass drum … 10 hi-hat) | ✅ |
| Channels 11 and up | ignored | ✅ |
| Note on | **Velocity is the channel volume.** Not multiplied by CC7 | 📏 |
| Note off | Almost absent (901 note-offs against 119,984 note-ons in the library, 0.75%). A note sounds until the next one | ✅ |
| Program change | Not a GM number but **a slot number in the song's own instrument records.** Loads the instrument only; the sounding note is not re-struck (`FUN_1bd4_0142`) | ✅ |
| CC7 | Applied directly as the channel volume, the same as velocity | 📏 |
| Pitch bend | NORE45 **widens it 2.5 times** before passing it on (below) | ✅ |

✅ **Bend widening** (`FUN_1bd4_0105`):

```
value passed = ((bend − 0x2000) × 5 + 0x3FFC) / 2
```

In the original code it appears as an OR with `0xE000`, because the deviation
is formed in 16-bit arithmetic. Using the raw value makes every bend 2.5 times
too shallow. The `.GYB`→`.OKA` converter wrote bends at 300 per step; widened
2.5 times that is 750, close to `.GYB`'s 819 per step.

✅ **Channel 10 is the hi-hat, not a lyric marker.** `SOVIRGIN.OKA`'s channel 10
carries 1,488 note-ons on notes 42/46 (closed/open hi-hat) and 890 program
changes alternating between its `CLHIHAT0` and `OPHIHAT` slots.

✅ **Not re-striking on an instrument change matters even more than in GYB.**
With almost no note-offs, 3,282 of 4,018 program changes (82%) arrive while a
note is sounding. Re-striking puts a stray hit into 60 of 63 songs.

The instrument record layout and the handling of empty records are in
[opl-instrument.md](opl-instrument.md). Opposite to GYB, `.OKA` reads
**NORE.BNK first, GAYO.BNK second.**

📏 1,092 of the 1,638 `.OKA` slots (67%) are empty records. Without the two bank
files (`GAYO.BNK`, `NORE.BNK`), two thirds of `.OKA` slots cannot sound.

## 4. Known differences and unknowns

- A `.OKA` converted from the `.GYB` of the same song has **every rest
  missing** ([gyb.md](gyb.md) section 9). Not a player problem.
- ❓ The fifth byte of a sync record.
- ❓ What the spare block (`[0x1B4]` bytes) is for.
- ❓ The format of the `.OKW` recording (only the sample-rate field is confirmed).
