# `.MDZ` — MLD (Sharp X68000, FM + PCM + MIDI together)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: the MLD package's **MML compiler `mlc.x`** and decompiler **`mdz2mus.x`** (`MLD247.LZH`, NFG Games' x68pub mirror, `SOUND/MLD/`), run on Windows under run68x. MML compiled and the bytes read; 176 songs decompiled, recompiled and compared track by track on tick counts.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../mdz-mld.md)

**MLD** is an X68000 music driver by LUM2 (1990-93). It drives the OPM (FM),
ADPCM and **an external MIDI module, all at once within one song.** Its manual
calls it an "MXDRV + MDD-like" driver, and that is the trap: **the header,
the offset table and the `F1 00` end marker match MDX, but the commands do
not.** The extension says nothing either — the same collection holds FM-only
`.mdz` files and `.mdx` files that are really MLD — so it has to be decided
**from the content.**

All numbers are **big-endian**.

## 1. The answer key — the compiler itself

The MLD package ships **the compiler that produced these files, `mlc.x`**, the
decompiler `mdz2mus.x`, and a 57 KB manual, `mlc.doc`. Both programs are
Human68k console tools, and **`run68x`** (github `kg68k/run68x`, win-x64 build)
runs them on Windows with no ROM and no operating system. So any question about
this format is answered by **writing MML, compiling it, and reading the bytes.**

```
A C1     V105   @v115  p64    @16    @t228  y91,40    o4 c4  r4
  E1 10  E3 16  FB 8C  FC 40  FD 10  FF E4  FE 5B 28  3C 30  80 30
```

⚠️ **Run it from a short directory.** Human68k limits path length, and from a
long path every call fails with `ファイルのパス名が長すぎます` — which looks
like a broken emulator.

`mlc.x -n file.mus` prints **each track's clock (tick) count**, and that is the
ground truth for timing. Decompiling 176 songs, recompiling them and comparing
against our own per-track tick counts took the match from **65.9%** to **94.5%**.

## 2. Header — the offset table comes in two widths

```
title  0D 0A 1A  PDX name  00  [offset table]
offset table = voice-table offset, track offset × N   (relative to the table)
```

✅ **The table entries are 2 bytes in some songs and 4 bytes in others**, and
nothing in the file says which. The 4-byte form is the same table with a zero
high word on every entry. Try 2 bytes first, then 4.

> Before the 4-byte form was known, those songs **loaded without error and played
> silence.** 18 of the 176 songs in the older collection (including ten identical
> `Bahnwelt*gs` files) and 7 GRADIUS II songs were like that. For a format whose
> failure is silence, test "does anything come out", not "does it load".

Finding the table's start works as for MDX ([mdx.md](mdx.md) section 1).

## 3. What a track drives — `0xE1`

✅ A track names its target with `0xE1`. **Bit 4 of the operand is the whole
distinction.**

| MML | Bytes | Target |
|---|---|---|
| `C1`-`C16` | `E1 10`-`E1 1F` | MIDI channel 1-16 (`0x10 \| (ch−1)`) |
| `CH1`- | `E1 00`- | OPM (FM) channel |
| `CHP1` | | ADPCM |

✅ Look only at the start of a track, but **skip exclusives (below) whole**
before counting 64 bytes: some tracks spend their first 64 bytes inside a 1.5 KB
exclusive. Stopping at the first byte below 0x80 instead reads operands as notes
and misses 19 of 27 FM songs.

✅ A track can change its target part way through with another `0xE1`.

Of 158 songs in the older collection that use MIDI, **146 are MIDI-only** and
**12 mix FM/PCM with MIDI.** The titles say so outright: `For SC55+X680x0`,
`for CM-64+PCM8`, `SC-55+OPM+ADPCM`.

## 4. Notes and time

- ✅ Notes are **raw MIDI note numbers** (`c4` → `3C` = 60) plus a length byte.
  `0x80` is a rest.
- ✅ **Quarter note = 48 ticks** (`r4` → `80 30`).
- ✅ **FM tracks use the same command bytes.** `CH1 c4` is `E1 00 3C 30`. So what
  the FM side needs is **an output stage**, not a new sequencer.
- ✅ **FM pitch = MXDRV note number (MIDI note − 15), key fraction `0x14`.** The
  X68000 clocks its OPM at 4 MHz while the chip's key codes are named for
  3.58 MHz, a 1.92-semitone difference; MXDRV absorbs it in its note numbers and
  a key fraction of 5/64 (MDX note 45 plays 261.1 Hz = MIDI 60). MLD writes `c4`
  as 60 on FM tracks too, so −15 is what puts it **at the same pitch as the MIDI
  part.** The songs themselves are the evidence: `ken_pcm`'s FM tracks carry
  the same notes as its MIDI tracks, one for one. A driver built to double a
  MIDI module with the OPM has to make `c4` the same pitch on both.
- ✅ **The driver's initial tempo is timer B `0xC8`** (`move.b #$C8,$BA(a5)` in
  three places in `mld.x`'s resident code), about 87.2 BPM. Everything before
  the song's first `@t` runs at this tempo. Using the Standard MIDI File default
  (120 BPM) puts the MIDI part ahead of the FM part in songs whose first `@t`
  comes late (0.174 s in the GRADIUS II set).

## 5. MIDI-side commands

| Command | MML | Meaning | |
|---|---|---|---|
| `0xE1` | `C` / `CH` / `CHP` | target (section 3) | ✅ |
| `0xE3` | `V` | Main volume → CC7. **An attenuation: CC7 = 127 − n** | ✅ |
| `0xFB` | `v` / `@v` | Velocity. Bit 7 set: `255 − n`. Clear: 16-step curve (appendix B) | ✅ |
| `0xFC` | `p` | Pan → CC10 (as is) | ✅ |
| `0xFD` | `@` | Program (as is) | ✅ |
| `0xE2` | `P` | Sustain → CC64 (as is) | ✅ |
| `0xFE` | `y` | Control change, 2 bytes | ✅ |
| `0xD6` | | **An NRPN group**, 3 bytes → CC99 / CC98 / CC6 | ✅ |
| `0xFF` | `t` / `@t` | Tempo = OPM timer B (larger is faster) | ✅ |
| `0xF8` | `q` / `@q` | Gate. `q1`-`q8` are eighths of the note; `@qN` is stored as `255 − N` clocks | ✅ |
| `0xF6` `0xF5` `0xF4` | `[` `]` `/` | Loop start, end, escape. **Signed relative jumps** | ✅ |
| `0xF7` | `&` | Tie. Marks **the note that follows** | ✅ |
| `0xD7` `0xCA` | `EX` / `WEX` | Exclusives, running to `0xF7` | ✅ |
| `0xBD` `0xBE` `0xBF` | module macros | Rebuilt as Roland SysEx (below) | ✅ |
| `0xD0` | `DV` | Device ID (default 17 = `0x10`) | ✅ |
| `0xF1` | `L` / end | `00` ends the track; otherwise an infinite-loop jump | ✅ |

✅ **Module macros rebuild exactly, checksum included.** `#SC:EM1` compiles to
`BE 40 01 30 01 0E F7`. `0x40+0x01+0x30+0x01 = 0x72` and
`128 − 0x72 = 0x0E`, so what is stored is **the Roland message from the address
through the checksum**, and the driver adds only the `F0 41 <device> <model> 12`
head.

| Command | Model | Target |
|---|---|---|
| `0xBD` | `0x45` | SC-55 display |
| `0xBE` | `0x42` | GS |
| `0xBF` | `0x16` | MT-32 / CM-64 |
| `0xB7` `0xBB` | ❓ | Not sent — model byte unknown (a wrong model is worse than silence) |

✅ **Exclusives and module macros go out on MIDI whatever the track drives**,
because an FM or PCM part has nowhere else to send them. `NEW_Wa`'s first track
is its PCM part (`CHP1`), yet it carries an MT-32 reset, a partial reserve and
**an upload of five CM-32L timbres.** Sending exclusives from MIDI tracks only
leaves this song's MIDI half (nine parts on memory timbres) entirely silent.

> The first diagnosis was, wrongly, "the timbre data is missing from the
> collection". It searched our **converted output** for uploads — which the
> converter had dropped, so of course there were none. **Decompile the source
> file (`mdz2mus`) before concluding what it does or does not carry.**

## 6. The FM / PCM side

- ✅ `@5`, `@v100`, `V100`, `p2`, `@q7` compile to the same commands with the same
  attenuation arithmetic as on the MIDI side. Differences: `p` is the stereo bits
  of OPM `0xC0` (0-3), and `y` is a **direct register write** rather than a
  control change.
- ✅ `0xEA` = **`MH`, the hardware LFO, five operands.** Byte for byte MXDRV's
  long `0xEA` (`MH2,207,4,0,0,0,0` → `EA 02 CF 84 00 00`). `E8 10` / `E8 00` are
  `MHON` / `MHOF`.
- ✅ `0xEC` = an LFO setup, **five** operands (not three).
- ✅ `0xED` = MDX's `F` (ADPCM rate), one operand. It opens PCM tracks as `ED 04`.
- ✅ The PCM output goes through the same X68000 filter as MDX
  ([mdx.md](mdx.md) section 8).

> With `0xEC` read as three operands and `0xED` as none, `ken_pcm`'s FM track
> started 0.88 s late and its drum tracks 1.56 s late. Fitting against tick
> totals cannot catch this — a linear walk resyncs on the next command either
> way. Look at **whether the byte right after the command makes sense**: with
> `0xEC` at five, the next byte is always `E9`/`E8`/`80`; at three it was
> 0x00-0x05 (an inaudible note).

## 7. Keeping FM and MIDI together (how it was checked)

Whether the two halves sit on the same beat was checked by **matching FM key-on
times against MIDI note-on times in 10-second windows.** A correctly doubled
song reads an offset of 0 ms in every window; a fault shows as a slide from
window to window.

✅ All 20 mixed songs across both collections (183 songs) read **0 ms in every
window, with no drift.** The 16 that round-trip through the compiler **match
its tick count on every track.** (`NEW_Wa`, `Prel_le` and `defeat` cannot be
checked that way: `mlc` rejects the MML `mdz2mus` writes for them.)

## 8. Unknown

- ❓ 13 kinds of command are still unidentified (consumed with no operands).
  5.5% of tracks disagree with the compiler's tick count. No single command
  length improves it further, so what is left looks structural.
- ❓ `0xCD`, `0xD8`: no MML probe produced them; their length was fitted on
  tick counts.
- ❓ How a PCM track's note number maps to a PDX sample. `ken_pcm`'s PCM tracks
  play notes 36 and 38 — **General MIDI's kick and snare** — which looks like a
  lookup table rather than a plain index.
- ❓ The banks songs ask for — `ADT00SC.PDX`, `gr3_p.pdx`, `dsp.pdz`,
  `ken.pdz` — could not be found.
- ⚠️ `ANGE10.mdz` has a loop that closes on itself; read at a wrong length it
  goes round 16,001 times. Read the unknown-command census **per file** — as a
  total, this one song swamps everything.

---

## Appendix A. Operand counts per command (`0x80`-`0xFF`)

Obtained by compiling MML with `mlc.x`. `-1` means unknown (treated as 0).
`0xD7`, `0xCA`, `0xB7`, `0xBB`, `0xBD`-`0xBF` run to `0xF7` (variable length);
`0xF1`/`0xF4`/`0xF5`/`0xF6` carry jumps and are handled separately.

```
        0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F
80     -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1  -1
90     -1  -1  -1  -1  -1  -1  -1  -1  -1  -1   1   2  -1   2  -1   0
A0      0   0  -1  -1   2  -1  -1   0  -1   5   7   5   2   2   1  -1
B0      5   1   1   2   2  -1  -1  -1  -1   0   0  -1  -1  -1  -1  -1
C0     -1   2   1   1   2   0   1   0   0   3  -1   1  -1   1  -1  -1
D0      1  -1  -1  -1   0  -1   3  -1   1   1   1   0  -1   2  -1   1
E0      0   1   1   1   1  -1   0   1   1   2   5  -1   5   1   0   1
F0      1  -1  -1  -1  -1  -1  -1   0   1   1   1   1   1   1   2   1
```

Commands and their MML names (confirmed by compiling):

```
9A BS~/BS_        9B @F fade        9D @BS            9F VC restore
A0 @vC restore    A1 tC restore     A4 @t+/-          A7 BOF
A9 MA LFO         AA MP (4 args)    AB MP LFO         AC @_ portamento
AD _ portamento   AE MZS            B0 MZ volume LFO  B1 p-
B2 p+             B3 D~/D_          B4 D detune       B9 #SC:INIT
BA #CM64:INIT     C1 (2 args)       C2 V-             C3 V+
C4 @_ portamento  C5 R marker       C6 J              C7 APOF
C8 APON           D0 DV device ID   D4 RA             D6 NRPN
D9 BS             DA BR             DB MOF            DD ?
DF O#             E0 RT             E1 C target       E2 P sustain
E3 V volume       E4 K/T            E6 ` key off      E7 ?
E8 MPON/MAON/MHON E9 M modulation   EA MH (5)         EC LFO (5)
ED F PCM rate     EE W wait         EF S sync
F0 k delay        F7 & tie          F8 q/@q gate      F9 ( velocity down
FA ) velocity up  FB v/@v           FC p pan          FD @ program
FE y control      FF t/@t tempo
```

## Appendix B. `v0`-`v15` → velocity

```
0, 7, 15, 23, 31, 39, 47, 55, 64, 71, 79, 87, 95, 103, 111, 119
```
