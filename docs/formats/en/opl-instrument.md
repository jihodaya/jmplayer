# The GYB · OKA instrument record, `.BNK` banks, built-in instruments

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: decompilation of GAYOBANG.EXE · NORE45.EXE, measurements over 169 `.GYB`/`.OKA` songs (772 instrument records), comparison against real-hardware recordings.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../opl-instrument.md)

Gayobang `.GYB` and Oksori `.OKA` use **the same 38-byte instrument record**.
The song carries its own instruments, and the original programs play them as
they are.

## 1. The 38-byte record

| Offset | Size | Contents |
|---|---|---|
| `+0` | 9 | Instrument name (ASCII, NUL-terminated or filling all 9) |
| `+9` | 1 | **Flag** (section 2) |
| `+10` | 28 | OPL parameters (section 3) |

✅ GYB and OKA records are byte-identical: in a `.GYB`/`.OKA` pair of the same
song, all 17 slots matched.

## 2. The flag byte — two meanings

```
flag = (MT-32 tone number << 1) | paramsValid
```

- ✅ **Bit 0 = are the 28 parameter bytes real.** The correlation over 772
  records is exact: with bit 0 clear, all 28 bytes are **zero**. Such
  "name-only" records are common in songs converted from `.ROL` — all 400
  `.OKA` slots of that kind, and 32 of 334 `.GYB` songs. Left alone they are
  silent, because an attack rate of 0 never rises.
- ✅ **Bits 1-7 = the MT-32 tone somebody chose when playing the song through
  a MIDI module.** GAYOBANG finds its name at
  `(flag >> 1) × 0x0F + tone table + 3`. The tone table sits at file offset
  **`0x3924B`** in GAYOBANG.EXE: 128 entries, alternately 14 and 15 bytes
  apart, and it is **the MT-32's preset list verbatim**. Decoded across the
  library it reads `elviolin`→violin1, `accordn`→Accordion,
  `snare1b`→Deep Snare, `bells`→Water Bells — choices somebody made by ear in
  the 1990s.

> For a while the values other than 1 were dismissed as "noise", because few
> files carry them (10 `.GYB`, 2 `.OKA`). The lesson: **a distribution cannot
> tell you what a byte means.**

## 3. The 28 OPL parameter bytes

The same as an AdLib Visual Composer `.BNK` instrument minus its first two
bytes (mode, voice number). 13 bytes per operator, in the order of AdPlug's
`read_fm_operator()`.

| Offset | Modulator | Offset | Carrier | Meaning |
|---|---|---|---|---|
| 0 | KSL | 13 | KSL | key scale level |
| 1 | MULT | 14 | MULT | frequency multiple |
| 2 | FB | 15 | FB | feedback (**only the modulator's is used**) |
| 3 | AR | 16 | AR | attack |
| 4 | SL | 17 | SL | sustain level |
| 5 | EG | 18 | EG | sustaining or not |
| 6 | DR | 19 | DR | decay |
| 7 | RR | 20 | RR | release |
| 8 | TL | 21 | TL | output level |
| 9 | AM | 22 | AM | tremolo |
| 10 | VIB | 23 | VIB | vibrato |
| 11 | KSR | 24 | KSR | key scale rate |
| 12 | CON | 25 | CON | connection (0 = additive) |
| 26 | waveform (modulator) | 27 | waveform (carrier) | **low 2 bits only** |

✅ The carrier's FB field is unused: GAYOBANG writes register `0xC0` from the
modulator only. Values outside 0-7 are common there, and using them gives the
harsh edge of maximum feedback.

✅ The waveform bytes must be cut to 2 bits. 53 of the library's 403 slots
carry a value outside 0-3; read as 3 bits in OPL3 mode they select waveforms
the original could never produce.

## 4. How empty records are filled

✅ The original looks up **only empty records (bit 0 clear)** in a bank, by
name. GAYOBANG's `FUN_28a1_0155` binary-searches the bank's 12-byte name
entries; on a hit it copies 28 bytes and sets the flag, **on a miss it returns
−1.** There is no nearest-name fallback.

There are two banks:

| File | Belongs to | Instruments |
|---|---|---|
| `GAYO.BNK` | Gayobang | 2,154 |
| `NORE.BNK` | Oksori NORE45 | 6,009 |

- ✅ The two banks share every name but one, but **244 of the shared names
  hold different parameters**, so which bank is read first changes the sound.
  **The program's own bank comes first** (`.GYB`: GAYO then NORE; `.OKA`:
  NORE then GAYO).
- ✅ The 15,867-instrument general OPL collection `STANDARD.BNK` is
  **deliberately left out.** Neither DOS program ever read it, and filling
  from it gives a thin, bright voice the original never produced
  (measurements in section 5).
- 📏 In 169 songs, 96 carry 1,121 empty slots, and the two banks fill 1,113
  of them. The remaining 8 are in 4 songs (`THDRFOS1.GYB`,
  `STATION.GYB`/`.OKA`, `NOISENEW.OKA`).

### `.BNK` layout (AdLib Visual Composer)

```
0x0A  u16  total entries
0x0C  u32  offset of the name list
0x10  u32  offset of the data
name entry, 12 bytes: u16 data index, u8 used, 9-byte name
data entry, 30 bytes: u8 mode, u8 voice number, 28 parameter bytes (as in section 3)
```

## 5. The six built-in instruments

✅ GAYOBANG.EXE holds six 28-byte instruments at `0x445a:0x120a`. Before a
song plays, melodic channels (0-5) get number 0 and drum channels (6-10)
numbers 1-5 (`FUN_255a_101b`).

```
0: 01 01 03 0f 05 00 01 03 0f 00 00 00 01  00 01 f6 0d 0f 00 02 02 00 00 00 01 01  00 00
1: 00 00 00 0a 04 00 08 0c 0b 00 00 00 01  00 00 2f 0d 04 00 06 0f 00 00 00 00 01  00 00
2: 00 0c 00 0f 0b 00 08 05 00 00 00 00 00  00 00 2f 0d 04 00 06 0f 00 00 00 00 00  00 00
3: 00 04 00 0f 0b 00 07 05 00 00 00 00 00  00 00 2f 0d 04 00 06 0f 00 00 00 00 00  00 00
4: 00 01 00 0f 0b 00 05 05 00 00 00 00 00  00 00 2f 0d 04 00 06 0f 00 00 00 00 00  00 00
5: 00 01 00 0f 0b 00 07 05 00 00 00 00 00  00 00 2f 0d 04 00 06 0f 00 00 00 00 00  00 00
```

(each row: 13 modulator bytes, 13 carrier bytes, 2 waveform bytes)

✅ The table is exactly six entries. The five record-sized slots just before
`0x120a` decode to values outside the OPL ranges, so the table does not extend
further back.

### An empty record no bank carries — the one place the port knowingly differs from the original

The original's code loads a **built-in instrument** into an empty record that
no bank carries. Which one is decided not by the slot but by **the channel
playing it** (0 for melodic, 1-5 for drums). JMPlayer deliberately differs
here and **keeps the instrument the channel was already playing**, because
measurement put that closer to recordings of the original.

Band-profile distance from real-hardware recordings (lower is closer):

| Song (band compared) | Keep previous | Built-in | Fill from another bank | Silent |
|---|---|---|---|---|
| `THDRFOS1.GYB` `bassbel3` (160 Hz-5.1 kHz) | **2.59 dB** | 5.24 dB | 4.91 dB (STANDARD.BNK) | 4.68 dB |
| `ZEL#3.GYB` 5 empty slots (80 Hz-10.2 kHz) | **0.62 dB** | 0.67 dB | 0.73 dB (NORE.BNK) | — |

On `ZEL#3` the three are within 0.11 dB — effectively **a tie.**

📏 The built-in never won. But why the original's code disagrees with the
recording is not explained. Everything checkable was checked: the record
really is 28 zero bytes, the DOS install's bank really lacks the name, the
built-in table really is six entries. ❓ **More real-hardware recordings of
songs with empty records could settle it.**
