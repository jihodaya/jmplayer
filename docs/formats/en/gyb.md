# `.GYB` — Gayobang karaoke (GAYOBANG)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: decompilation of GAYOBANG.EXE, comparison against recordings of the original program, measurements over 334 songs in the library.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../gyb.md)

The song file of **Gayobang** (GAYOBANG.EXE), a Korean DOS karaoke program of
the 1990s. It plays through an AdLib (OPL2/OPL3) FM chip and carries its own
**instrument data and lyrics** inside the file. It shares **one OPL driver**
with the Oksori karaoke program (NORE45), so most of the playback rules below
apply to `.OKA` unchanged ([oka-okm-okw.md](oka-okm-okw.md)).

---

## 1. Overall layout

```
0x00            header (64 bytes)
0x40            (magic 0x04 only) 15-byte secondary header
0x40 or 0x4F    global events (tempo)
                channel blocks × 11 (channels 0-10)
                instrument records × N (38 bytes each)
                lyrics (to end of file)
```

✅ A file with magic `0x04` carries a 15-byte secondary header after the
64-byte header, so its body starts at `0x4F`; a `0x03` file starts at `0x40`
(GAYOBANG.EXE `FUN_28a1_113b`, `DAT_445a_8d6a = 0x0F`).

## 2. Header

| Offset | Size | Contents | |
|---|---|---|---|
| `0x00` | 1 | Magic: `0x03` or `0x04` | ✅ |
| `0x01` | 25 | Song title, **Johab-encoded Korean**, NUL-terminated | ✅ |
| `0x28` | 1 | tickBeat divisor (usually 4). Part of the playback rate (section 4) | ✅ |
| `0x2A` | 1 | Time-signature numerator. **Also the number of beats per lyric line** (section 7) | ✅ |
| `0x2B` | 1 | Time-signature denominator (4) | ✅ |
| `0x2D` | 1 | **Rhythm-mode flag.** Non-zero: OPL rhythm mode (11 channels); zero: 9 melodic channels | ✅ |
| `0x2E` | 1 | **Key centre** (usually 60). Sounding note = command − `[0x2E]` + 60 | ✅ |
| `0x32` | 2 | Looks like "tempo" but **is not a tempo**: it reads 100 in songs that play at 60 BPM and at 200 BPM | ❓ |
| `0x34` | 4 | **Base tempo × 100** (e.g. 14000 = 140 BPM) | ✅ |
| `0x3C` | 2 | Number of instrument records | ✅ |

The positions of `0x2D`/`0x2E`/`0x32` were confirmed from GAYOBANG keeping the
song header at `0x8d2c` in memory and passing `0x8d59` (= file `0x2D`) to its
rhythm set-up routine `FUN_255a_1222`. The neighbouring `0x8d5a` holds 60 and
`0x8d5e` holds 100, which pins the base address. ✅

## 3. Body

### 3-1. Global events (tempo)

```
u16  count
[ u16 tick, u16 value ] × count
```

✅ The value is a **percentage of the base tempo.** 100 is the header tempo
as is; 50 is half speed.

✅ **A value of 1000 is a tempo.** 8 of the 27 songs in the library put 1000
at tick 0. GAYOBANG prints it on its score as "♩=1400" (for a 140 BPM song)
and really does rush through at that speed. It is inaudible not because of the
tempo but because every channel sits at volume 0 for that stretch (volume 0 at
tick 0, the real volume at tick 32). Skipping or special-casing the range
starts the song about 1.5 s off.

### 3-2. Channel blocks (channels 0-10, in order)

```
u8   channel number (must run 0, 1, 2 ...)
u16  end tick of this channel
[ u8 command, u8 length ] ...    until the lengths add up to the end tick
u16  instrument-change count   [ u16 tick, u8 slot, u8 0 ] × count
u16  volume count              [ u16 tick, u8 volume, u8 0 ] × count
u16  pitch count               [ u16 tick, u8 pitch, u8 0 ] × count
```

✅ This matches the order in which GAYOBANG's `FUN_28a1_18f0` reads it.

**Note commands (command, length)**

| Command | Meaning | |
|---|---|---|
| `0x00` | Rest. Keys off the sounding note | ✅ |
| `0x01`-`0x78` | Note on. Sounding note = command − `[0x2E]` + 60 | ✅ |
| `0x79` and above | see below | ❓ |

❓ **`0x79` and above has two competing readings.** JMPlayer's playback engine
reads it as "keep the current note sounding (do nothing)"; the jmpconv
converter reads it as "key off note (value − 0x79)". jmpconv matched all
74,566 note events of its reference conversions, but no song has yet been
found where the two readings actually sound different. Both assume one note
per channel, so on most songs they give the same result.

Lengths are in ticks; a command longer than 255 ticks is written as the same
command repeated.

**Volume**: file value 0-100. GAYOBANG computes
`channel volume = master × value / 100` (clamped at 127) and applies it to an
operator's TL **linearly** (`FUN_255a_0245`, `FUN_255a_082e`):

```
TL' = 63 − ((volume × (63 − TL) + 64) >> 7)
```

✅ This is not a logarithmic curve with a dB table. Which operators it applies
to is in section 5, item 4.

**Pitch bend**: file value 0-20, **10 is centre**. GAYOBANG passes
`value × 0x333` (819 per step) to the AdLib library's 14-bit bend
(centre `0x2000`) (GAYOBANG.c:30115). ✅

### 3-3. Channel layout and rhythm mode

In rhythm mode (`[0x2D] ≠ 0`):

| Channel | 0-5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|
| Role | melodic | bass drum | snare | tom | cymbal | hi-hat |

✅ With `[0x2D] = 0` there is no rhythm mode and 9 melodic channels. One song
in the library, `WITHLOVE.GYB`, is like this.

## 4. Playback rate

✅ How fast the ticks run:

```
BPM          = ([0x34] / 100) × (global event value / 100)
ticks/second = BPM × [0x28] / 60
```

The same shape as the formula AdPlug uses for IMS (`basicTempo × tickBeat / 60`).
`[0x28]` is easy to miss: every sample song had 4 there, so for a while it was
mistaken for a constant.

## 5. What the original driver does — to reproduce it faithfully

Behaviour confirmed by decompiling GAYOBANG's OPL driver. Only the points where
it **differs** from a typical AdLib player (such as AdPlug's composer) are listed.

1. ✅ **It plays the instruments inside the song.** Do not look slot names up
   in a bank and overwrite them. A DOS GAYOBANG with its bank file deleted
   still plays the whole library normally. Looking names up in a bank
   **changed instruments in 88% of songs.** A bank is consulted only for an
   empty record ([opl-instrument.md](opl-instrument.md)).
2. ✅ **An instrument change does not re-strike the sounding note.** Only the
   instrument registers change; the envelope carries on. Re-striking adds an
   extra hit on every change (`FUN_255a_06ad` / `_07df` are not wrapped in a
   key off/on).
3. ✅ **The frequencies of the two shared rhythm channels are set once, when
   the song starts.** Channel 7 (snare + hi-hat) gets note 31 and channel 8
   (tom + cymbal) note 24, through GAYOBANG's own note table. The result is
   **fnum 517 / block 2 = 98.0 Hz** and **fnum 690 / block 1 = 65.4 Hz**.
   AdPlug puts these two channels exactly **one octave higher** (195.3 Hz /
   130.1 Hz). When a drum is struck, GAYOBANG touches only the key bits of
   register `0xBD` and never rewrites the frequency.

   > Reading the note table: one word per semitone at `0x445a:0x143a`, a
   > per-note block at `0x15ba`, a row index at `0x161a`. **A word with its top
   > bit set means "the F-number for the next block up"** — the part that is
   > easy to misread.
4. ✅ **An additive instrument has its modulator attenuated too.** GAYOBANG
   applies channel volume to an operator if it is the carrier, OR its own
   connection bit is 0 (additive), OR it is a rhythm drum operator. AdPlug
   attenuates the carrier only. 133 of the 403 instruments in the library
   (33%) are additive.
5. ✅ **The feedback/connection register (`0xC0`) is written from the
   modulator.** `FUN_255a_097c` writes `0xC0` only for an operator whose
   "is carrier" entry (`DAT_445a_1363`) is 0. The carrier's feedback field
   is filler (5,268 of the bank's 6,009 carrier bytes fall outside 0-7).
6. ✅ **Register `0x08` is set to `0x40` (NOTE-SEL) when a song starts.** It
   changes the key scaling of envelope rates. AdPlug's initialisation leaves
   that bit clear.
7. ✅ **Only two bits of the waveform are used.** The instrument loader ends
   with `record[13] = waveform & 3`. Passing three bits to an emulator in OPL3
   mode selects waveforms the original could never produce.
8. ✅ **Initial instrument per channel**: before a song plays, the melodic
   channels (0-5) get built-in instrument 0 and the drum channels (6-10)
   built-in instruments 1-5 (`FUN_255a_101b`).
9. ✅ **Doubling in OPL3 mode**: on an OPL3 the second register set
   (port `0x38a`) plays the same note again with the bend raised `0x600`
   (+18.75 cents), and above mode 3 at 9/10 amplitude. It is real, but worth
   only 0.08 dB of band balance, so JMPlayer does not implement it.

## 6. Instruments

`[0x3C]` 38-byte records follow the channel blocks directly. The record layout
and the handling of empty records are in [opl-instrument.md](opl-instrument.md).
Slot 0 is usually empty; the value of an instrument-change event is this slot
number.

## 7. Lyrics

✅ The lyrics run from just after the instrument records to the end of the file.

- **Domestic edition**: Johab-encoded Korean, **one line = a fixed 75 bytes**.
  One syllable is 2 bytes (two cells).
- **Export (English) edition**: plain ASCII, one byte per cell. **A run of 5 or
  more NULs is a line break.**

✅ **Lyric highlight speed** — the original shows one line at a time, and each
line stays for a fixed number of ticks.

```
ticks per line         = [0x28] × [0x2A] × 4       (FUN_28a1_0436, FUN_2dcf_2fe8)
tick of a byte position = position × [0x28] × [0x2A] × 4 / 75
```

So the highlight sweeps the 75 cells of a line left to right at a steady rate.
It moves **in beats**, independent of tempo. Fixing `[0x2A]` at 4 makes the
lyrics drift late in 3/4 songs (English `P_G_005`-`010`) and 2/4 songs
(`P_G_004`).

📏 JMPlayer adds a lead of 1.75 beats for English lyrics and 1 extra beat at
the first syllable of each line. Those were set by ear.

## 8. `GYB.LST` — the song list

A list file that sits in the song folder. It is where a title is found when
the song file itself carries none.

```
0x00  8-byte header
0x08  records × N, 40 bytes apart
        +0   24  title (Johab)
        +24  12  file name (8.3, ASCII)
        +36   4  ❓
```

## 9. Why the `.OKA` of the same song sounds different

The same song often exists both as `.GYB` and `.OKA` (one converted from the
other). When they sound different, it is **the file, not the player.**
Measured on `BACKPUSA`:

- The note-on count (3,577) and the 17 instruments are **byte-identical.**
- The `.GYB` holds **1,994 rests**; the `.OKA` holds **zero** note-offs. The
  conversion dropped every rest, so notes keep sounding where the original
  was silent.
- As a result the `.OKA` measures 1.8-2.5 dB hot at 40-160 Hz.

The information is gone from the file; no player can recover it. When both
exist, play the `.GYB`.

The two programs also genuinely differ in bend depth: GYB is 819 per step
(`× 0x333`), OKA is 750 after NORE45 widens it 2.5 times. Both numbers come
from the original code, so **they really are different.**

## 10. Still unknown

- ❓ What header `0x32` is (always about 100).
- ❓ The exact meaning of note commands `0x79` and above (section 3-2).
- 📏 **Drums −4.5 dB**: lowering the five rhythm operators by 4.5 dB brings
  every band from 80 Hz to 12 kHz within 0.6 dB of a real-hardware recording.
  But it is **fitted to one recording of one song** and does not come from the
  original code. A real-hardware recording of a second song would settle it.
- ❓ Why the end of a song comes out about 5 dB quieter and 1.5 s shorter than
  on DOS.
