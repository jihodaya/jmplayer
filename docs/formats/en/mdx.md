# `.MDX` + `.PDX` — MXDRV (Sharp X68000)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: the command jump table in the MXDRV driver source (mxdrv200b), 101 songs compared against **mxwav** (the original driver + the X68Sound renderer), and hand-written one- or two-note MDX files rendered by the original.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../mdx.md)

The song file of **MXDRV**, the X68000's standard music driver. It drives the
YM2151 (OPM) FM chip, eight channels, and ADPCM (MSM6258) samples. The samples
live in a separate `.PDX` file.

MDX is well documented publicly. This document concentrates on **where common
readings turned out wrong when compared with renders by the original driver.**
All numbers are **big-endian**.

> **A note on method.** mxwav will render any MDX. A 66-byte file with one
> track, one voice and four notes is a valid MDX. Writing small files like that
> by hand and comparing the original's render with ours taught far more than
> comparing whole songs. Two cautions: the track data must start right after
> the offset table (the track count is derived from the first track's
> offset), and a track must never actually end — mxwav crashes — so park it on
> `7F F1 FF FD`, a rest that jumps back onto itself.

---

## 1. Header

```
title (Shift-JIS)  0D 0A 1A
PDX file name  00
[offset table]  u16 voice-table offset, u16 track offset × tracks   (relative to the table)
```

- ✅ **Track count = (first track offset − 2) / 2.** Do not fix it at 9: 95 of
  100 songs have 16 tracks (8 FM + 8 PCM).
- ✅ **The table's start has to be found.** It is usually right after the PDX
  name's NUL, but files such as `ff4_01jsc` have 8 bytes in between. Step
  forward two bytes at a time until every offset in the table points at or
  past the end of the table. Ordinary files match on the first try.

## 2. Voice record — 27 bytes

```
[0]      voice number
[1]      (FB << 3) | ALG
[2]      slot mask (which operators key on)
[3..26]  24 bytes: 6 parameter rows × 4 operator columns
         row order: DT1/MUL, TL, KS/AR, AMS/D1R, DT2/D2R, D1L/RR
```

- ✅ **The 24 bytes are parameter-major** (a row per parameter). Read as six
  bytes per operator, every envelope is scrambled into metallic noise. Decided
  by the register rules: TL is 0-127 so it never sets bit 7, and KS/AR,
  AMS/D1R and DT2/D2R leave bit 5 unused. Across 554 voices, parameter-major
  satisfies the rules 554 times, operator-major 49.
- ✅ **Voices are found by the number in their first byte**, not by position.
  Only 28 of 100 songs number them 0, 1, 2… without a gap.
- ✅ Operators go into the chip in register order (M1, M2, C1, C2) as they
  are. Reordering to MML order (M1, C1, M2, C2) measured worse.

## 3. Commands — notes and rests

| Byte | Meaning |
|---|---|
| `0x00`-`0x7F` | Rest. Length = **byte + 1** ticks |
| `0x80`-`0xDF` | Note (note = byte − 0x80). The next byte is the length. Length = **value + 1** ticks |
| `0xE0`-`0xFF` | Commands (section 4) |

✅ **Length and gate get one added** (driver `L001216`). A file of alternating
4-tick notes and 4-tick rests rendered by the original gives onsets
**10.01 ticks** apart (not 8). Without the +1 every song runs about **3% fast.**

> This +1 was tried twice and reverted twice, because the score being used —
> "is there a key-on of ours within 50 ms of each reference onset" — **scored
> higher the faster we played** (with 431 key-ons in 15 s, something always
> lands nearby). A wrong metric lets the wrong answer win.

## 4. Command table

Operand counts come from the driver's jump table (`mxdrv.cpp` line 5148,
indexed by `0xFF − command`), counting how many bytes each handler takes off
the stream. ✅

| Command | MML | Operands | Notes |
|---|---|---|---|
| `0xFF` | `@t` | 1 | Tempo = timer B. **Not end of track** |
| `0xFE` | `y` | 2 | Direct register write. **Register `0x12` changes the tempo too** |
| `0xFD` | `@` | 1 | Voice change (on a PCM track, the PDX bank; section 7) |
| `0xFC` | `p` | 1 | Pan |
| `0xFB` | `v` / `@v` | 1 | Volume (section 5) |
| `0xFA` | `)` | 0 | One volume step (section 5) |
| `0xF9` | `(` | 0 | One volume step (section 5) |
| `0xF8` | `q` | 1 | Gate ratio |
| `0xF7` | `&` | 0 | Tie (section 6) |
| `0xF6` | `[` | 2 | Loop start. **Writes the loop counter into the stream itself** (`move.b (a4)+,(a4)+`) |
| `0xF5` | `]` | 2 | Loop end (signed relative jump) |
| `0xF4` | `/` | 2 | **Loop escape** (section 6) |
| `0xF3` | `D` | 2 | Detune (signed 16-bit, 1/64 semitone) |
| `0xF2` | `_` | 2 | Portamento |
| `0xF1` | `L` / end | **1 or 2** | First operand `0x00`: end of track (1 byte); otherwise a loop jump (2 bytes) |
| `0xF0` | `k` | 1 | Key-on delay |
| `0xEF` | `S` | 1 | Sync send |
| `0xEE` | | 0 | Sync wait |
| `0xED` | `F` / `w` | 1 | PCM track: sample rate / format. FM track: noise (register `0x0F`) |
| `0xEC` | `@m` | **1 or 5** | Software pitch LFO. One operand if bit 7 of the first is set |
| `0xEB` | | **1 or 5** | Software volume LFO. Same rule |
| `0xEA` | `MH` | **1 or 5** | **Hardware LFO.** Same rule. With five: `1B` waveform, `18` frequency, `19` PMD, `19` AMD, `38+ch` PMS/AMS |
| `0xE9` | | 1 | LFO delay |
| `0xE8` | | 0 | PCM8 mode |
| `0xE7` | | **variable** | Sub-command table (below) |
| `0xE0`-`0xE6` | — | — | **Not commands.** Stop the track (below) |

✅ **`0xEA`/`0xEB`/`0xEC` are variable length.** Fixed at one byte, the long
form's other four operand bytes are played as music, which **sounds like the
tempo wandering.** With this fixed, all 101 songs came out in the right key.

✅ **`0xE7` is a sub-command table** (driver `L001694`). After a selector byte:

| Selector | Total operands |
|---|---|
| 0 | Not a command; the track stops |
| 1, 3, 5, 6 | 2 |
| 2 | 7 (a word and a long, or six skipped bytes when PCM8 is off) |
| 4 | 3 or 4 (a channel, then one note or rest written out in full) |
| 7 and up | Past the table; the track stops |

✅ **`0xE0`-`0xE6` are not commands.** All seven entries point at one routine
that swaps the track's read pointer for a canned `7F F1 00` (rest, end) and
gives up. Meeting one means **the stream is being read where the driver never
would**, which makes them a good check on operand lengths: with the lengths
above, not one of the 101 songs ever meets one.

## 5. Volume

✅ The volume byte is two things at once.

- **Bit 7 set** (`@v`): the low 7 bits **are the attenuation.** Almost every
  song uses this form (values cluster around 140-155).
- **Bit 7 clear** (`v0`-`v15`): an index into the driver's curve.

```
v0-v15 → attenuation: 2A 28 25 22 20 1D 1A 18 15 12 10 0D 0A 08 05 02
```

The attenuation is added to the TL of the algorithm's carrier operators only.

✅ **`(` and `)` step the two forms in opposite directions** (`L001328`/`L001344`).
A plain `v` counts up (louder) and stops at 15; an `@v` (bit 7) counts up
(quieter) and stops at `0xFF`. Stepping both the same way makes `)` louder on
`@v`, which is what nearly every song uses.

✅ **Every track starts at volume 8** (source line 2981, `S0022 = 0x08`).
Starting at 127 means bit 7 clear → curve index 15 → the loudest entry, which
is why drum tracks that never set their own volume came out at full blast.

## 6. Tie and loop escape

✅ **A tie (`0xF7`) prevents the key-off, not the key-on.** The driver checks
this flag in one place only: the gate countdown (around `L0011ce`). If a tie
skips the key-on instead, a track whose first note is tied **never sounds at
all.** If the note is already sounding it carries on; otherwise it is struck
normally.

✅ **Loop escape (`0xF4`)** (`L00139a`): the operand is a forward offset to the
loop end (`0xF5`). Follow that `0xF5`'s offset to the loop counter, and **on the
last pass (counter 1) jump to just after the `0xF5`**, skipping the rest of the
loop body. Ignored, a track with an escape runs long by the skipped tail on
every loop and **drifts against the other channels.** Of the 36 songs using
it, 10 drifted by 98-320 ms; after the fix all 36 sit within 0-4 ms.

> "The operand length is right, so ignoring it is harmless" does not hold for
> **commands that move the read position.** Jumps, loop escapes and
> conditionals change the timing when ignored.

## 7. PCM — `.PDX` and ADPCM

### PDX

```
[u32 offset, u32 size] × N     then the sample data
```

- ✅ **N need not be 96.** The file does not state it; count "how many entries
  fit before the first sample begins". `CAM2.PDX` has 288.
- ✅ **On a PCM track `@n` selects a bank**: sample index = **note + n × 96**
  (`L000f28`). Ignoring it gives the wrong samples to songs that play drums
  out of banks 1 and 2.

### ADPCM decoding

✅ Decode **the low nibble of each byte first.** The other way round the step
size runs away and the signal pins itself at ±2047 — about the right loudness
and the wrong waveform, a "smeared thud".

> An attempt to settle this by "how smooth is the signal" gave **the opposite
> answer**, because a runaway decode parked at the clamp is very smooth. What
> settled it was **encoding a 5 kHz sine both ways and rendering it with
> mxwav**: low-first comes out a 96% pure tone, high-first as garbage.
> (440 Hz cannot tell them apart — adjacent nibbles are too alike.) When a
> decoder is in doubt, **feed the original a signal whose answer you know.**

### `F` (`0xED`) — PCM format

| Value | Format |
|---|---|
| 0-4 | ADPCM 3.9 / 5.2 / 7.8 / 10.4 / 15.6 kHz |
| 5 | 16-bit PCM |
| 6 | 8-bit PCM |
| 7 | off |

### Volume and pan

- ✅ PCM volume is resolved the same way as FM to an attenuation, then through
  **a second table** to a 0-15 level; `0x2B` and above is silent. That level is
  then multiplied through the PCM8 table
  (`PCM8VOLTBL = {2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80}`, close to exponential).
- ✅ **PCM8 has one voice per PCM track, eight in all.** Sharing one voice
  means drums struck together never sum.
- ✅ **Pan uses the OPM's bit order: bit 0 is left.**

## 8. Sound — the chip and the output stage

- ✅ **Timer B**: one tick = `(256 − timerB) × 1024 / 4,000,000` seconds.
- ✅ **OPM clock**: one call to Nuked-OPM's `OPM_Clock` is **one internal
  cycle**, and 32 of them produce one sample. The real chip produces a sample
  every 64 master clocks (the master clock is halved before the slot counter).
  Using one constant for both gives sound **an octave high with envelopes twice
  as fast.**
  > Scored by pitch-class profile (chroma) alone, an octave is invisible. This
  > error survived a month with a 101-song chroma of 0.915. Rendering a
  > one-note MDX beside the original exposed it at once: peaks at 110 Hz
  > against 214 Hz.
- ✅ **Register writes must not eat audio.** The chip has to run between the
  address and the data write; throwing away the samples produced meanwhile
  loses two samples per write. Queue the writes and drain them while rendering.
- ✅ **X68000 PCM output filter** (X68Sound): two high-passes per PCM8 channel
  (326 Hz, 61 Hz), then a low-pass on the sum (a 3.6 kHz biquad at Q 0.71 and a
  one-pole at 11.8 kHz). A good part of the X68000 drum sound comes from this
  filter.
- ✅ **FM DC blocking**: heavy-feedback voices put out a large DC term. The
  board AC-couples the chip, and X68Sound blocks DC at 12 Hz.

## 9. Measuring length

📏 An MDX carries no length. Run it through with the chip off. **One pass** ends
when "every track has either ended or jumped back at least once".

✅ **Do not count a song that ends as one that loops.** An ended track also
satisfies "jumped back", so songs that simply end (`G2FENDV` and 16 others)
were measured at **twice** their length. Count it as a loop only if at least
one track actually jumped back.

⚠️ Do not stop a track when it loops. A track whose body is two bars jumps back
every two bars.

## 10. Unknown

- ❓ `0xE7` selector 4 ("run this on another channel") and `0xEF` (sync) could
  move the read position or the timing, and are not yet acted on.
- ❓ 5-10 kHz comes out about 3 dB, and 10-20 kHz about 7 dB, above the
  original. Nuked-OPM reproduces the real chip's floating-point DAC, X68Sound
  computes clean — a question of which to be faithful to.
- ❓ One song, `ssf2_57`, still runs about 3% fast.
