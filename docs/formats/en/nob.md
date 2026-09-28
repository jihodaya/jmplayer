# `.NOB` — Oksori karaoke (before 4.0)

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: measurements over the library's `.NOB` files — a regression between lyric position and the vocal channel's note times (2026-07-31).
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../nob.md)

The song file of the Oksori karaoke program before version 4.0. A short header
is followed by a **plaintext Standard MIDI File**, and the lyrics come after that.

## 1. Layout

```
0x00  u8    flag (0x08)
0x01  15    song title (EUC-KR — unlike the lyrics, which are Johab)
0x10  ...   settings ❓
0x45        Standard MIDI File (Format 1, "MThd")
            ... FF 2F 00  (end of the last track)
            NUL padding (usually more than 100 bytes)
            lyric block (to end of file)
```

✅ The header is always 69 bytes (`0x45`). If `MThd` appears at `0x45`, it is
a `.NOB`.

Some song folders also carry a separate title list, `NOB.LST`.

## 2. The lyric block — its shape is the timetable

`.OKM` keeps a separate table of ticks per syllable
([oka-okm-okw.md](oka-okm-okw.md)); `.NOB` has no such table. Instead, **the
lyric block is laid out like a piano roll.**

- Each character sits **in the cell for the moment it is sung.**
- Spaces and short NUL runs between characters are **the waiting time.**
- The long NUL padding at the front is **the intro.**
- A run of 5 or more NULs is a line break.
- A Korean (Johab) character takes 2 bytes, i.e. two cells.

📏 **Cells to ticks** (measured over the library, 2026-07-31):

```
tick = cell position × 20.44
```

- Cell positions count from 0 at the byte right after `FF 2F 00`. **The NUL
  padding counts too** — it is the intro's time.
- A line forced through the origin still fits with R² = 0.9990 (88% of files
  above 0.98). In other words, **cell 0 is song tick 0.**
- **20.44 ticks per cell** is the median over 574 songs whose vocal-channel
  note count matches their syllable count 1:1.
- It **does not depend on the MIDI resolution**: TPQN 120 songs give 20.50,
  TPQN 192 songs 20.44. The lyric roll was authored at one fixed resolution,
  which is how the DOS player could scroll it without knowing the tempo map.

> Header byte `0x28` once looked like a resolution field (correlation 0.66).
> But files sharing a value needed different scales, and files with different
> values needed the same one; using it put songs `0125` and `0127` ten seconds
> out. It was an artefact of the sample.

## 3. Unknown

- ❓ What the settings at `0x10`-`0x44` mean.
- ❓ Which constant in the original program 20.44 ticks per cell comes from.
  It was fitted by measurement only; the original executable was not decompiled.
