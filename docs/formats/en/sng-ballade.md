# `.SNG` — Ballade / ミュージくん / ミュージ郎 (PC-98)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: 17 reference MIDI files produced by running the DOS converter **SNG2S 3.3** (M. Saito, 1993) under msdos-player; fields found by patching part of a file and feeding it to SNG2S.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../sng-ballade.md)

The song file of **Ballade**, Dynaware's sequencer for the PC-9801, and of the
same engine inside Roland's ミュージくん / ミュージ郎 packages. It shares its
extension with Recomposer but is **an entirely different format**, told apart
by the first line of the file.

Result: **15 of 17 songs match SNG2S note for note.** The other two are
problems in the files (section 5).

## 1. The first line

```
"BALLADE SONG Ver. n.nn \r\n" <title> "\r\n\x1a"      (Shift-JIS)
```

| Version | Program | Parts |
|---|---|---|
| `Ver. 1.00` | Ballade / ミュージくん | 10 |
| `Ver. 2.00` | Ballade2 / ミュージ郎 | 10 |
| `Ver. 3.00` | ミュージ郎2 | 10 |
| `Ver. 4.00` | Ballade3 | 16 |

This line is the only version marker. All numbers are **little-endian**
(it is an x86 program).

## 2. Overall layout — a saved memory image

```
0x46        block-size table: (parts + 1) × u32   — the conductor first, then one per part
            settings area: 94 bytes (Ballade2 onwards) / 2 bytes (oldest files)
            conductor block
            part blocks × parts    — each 81 bytes larger than its table entry
```

✅ The file is **a straight copy of the program's memory (heap).** Every block
carries allocator slack and leftovers from earlier edits, so **read only up to
the "bytes used" field.** Reading to the end of a block plays the editor's
leftovers as music. Before that field was found, record first bytes seemed to
take all 256 values evenly; once found, 24,994 of the 25,000 non-note records
turned out to be the one rest command.

> If a field seems to take 200 different values, suspect slack (leftovers) first.

### Settings area

✅ Taking `S` as the end of the size table (`0x46 + 4 × (parts + 1)`),
**the song-wide transpose** (signed 16-bit) is at `S + 9`. Just before it are
the master volume and the default tempo.

## 3. Part block

| Offset | Size | Contents |
|---|---|---|
| `+0` | 81 | Allocator slack (leftovers) |
| `+81` | 12 | Part name (Shift-JIS) |
| `+94` | 1 | **Module**: 0 LA, 1 PCM, 2 LA rhythm, 4 GS, 5 GS rhythm |
| `+112` | 2 | Bytes used by area 1 |
| `+114` | 2 | Bytes used by area 2 |
| `+116` | 2 | Bytes used by area 3 |
| `+158` | … | Area 1, area 2, area 3 in sequence, then slack |

✅ Areas 1 and 2 are **two clocks over the same span**: their lengths **always
total the same.** The cheapest check that a part parsed correctly.

### Area 1 — notes, 8 bytes

| Byte | Contents | |
|---|---|---|
| 0 | Note number | ✅ |
| 1 | gate (note length; 0 means no note) | ✅ |
| 2 | step (to the next record, 1 byte) | ✅ |
| 3 | **Nudge, signed** — moves the note without moving the clock | ✅ |
| 4 | velocity | ✅ |
| 5-6 | Score engraving | ✅ |
| 7 | Flags. **Bit 7 = tied to the previous record** | ✅ |

✅ **Byte 3 is not the step's high byte.** Reading the step as 16 bits works on
most records and then meets `26 30 30 fe`, where `0xfe30` is 65,072 ticks. A bar
is 192 ticks, so the step is one byte. Reading byte 3 as a nudge took the
17-song match from **70% to 93%.**

✅ **Bit 7 tie**: one sound written as two note heads across a bar line.
Striking both adds a note and pushes everything after it late.

### Area 2 — controllers, 4 bytes

```
[status, step, data1, data2]
```

Status `0x00` **only waits.** `0xB0` (control change), `0xC0` (program) and
`0xE0` (pitch bend) are what they look like. ✅

### Area 3 — 6 bytes

A bar index. Not needed for playback.

In both areas **a first byte of `0xFC` ends the area.**

## 4. Time

- ✅ **Quarter note = 48 ticks.**
- ✅ **The song starts one bar in.** Ballade counts a bar in, and every stream
  is written relative to the end of that bar.
- ✅ **The song-wide transpose does not apply to rhythm parts (module 2, 5).**
  Rhythm-part note numbers are drum slots. The part header also has a
  plausible-looking byte there, but using it as a per-part transpose scores 0%.

### Conductor block

- ✅ A bar record (12 bytes) at block start `+77`: `u16 bar length (ticks)`,
  `u8 time-signature numerator`, `u8 denominator`.
- ✅ The tempo stream starts 12 bytes after the **last** occurrence of the heap
  chunk end marker `FF 7F FF FF FF FF`. Records are `[u16 step, u16 tempo value]`.

### The tempo value is a slider position

✅ Feeding SNG2S every tempo value from 1 to 127 gives a curve that climbs by
two and stalls once every eighth step:

```
BPM = 2v − ⌈v / 8⌉          61 → 114,  72 → 135,  91 → 170
```

A song in 3/4 measures one BPM higher for the same value; that is SNG2S's own
rounding. The real driver programs a timer, and a timer cannot know the time
signature, so one curve is used for every metre.

## 5. The two songs that do not match — file problems

- `MAGICAL0.SNG`: **truncated** at 126 KB; its rhythm part is half missing.
  Only the last block is allowed to run past the end of the file, so the other
  nine parts still play.
- `3X3EYES2.SNG`: matching SNG2S would need 10,029 ticks inserted part way
  through, and nothing in the file asks for it. The conductor says 97 bars and
  every part is 96, all self-consistent, while SNG2S produces 148. Left alone
  rather than guessed at.

## 6. Unknown

- ❓ The fields of the 94-byte settings area other than transpose, master
  volume and default tempo.
- ❓ The exact meaning of area 1 bytes 5-6 (engraving).
- ❓ Module values not in the table, such as 3.
