# Notes on Old Music File Formats

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya) · YouTube [@jjome_Plus](https://www.youtube.com/@jjome_Plus))
> **Project**: [JMPlayer](https://github.com/jihodaya/jmplayer) — a player for old computer music
> **First written**: 2026-09-28
> **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
> **Language**: English translation · [한국어 원문](../README.md)

This folder records what was worked out about the structure of old music file
formats while building JMPlayer. Most of these formats have no surviving
official documentation; they were worked out by decompiling the original
programs, or by comparing against output the original programs produced.
Programs age; these notes are left so that someone else can read the same
files again.

The Korean text is the original. Where the two ever disagree, the Korean
version is the reference.

## Credit and citation

These documents are published under **CC BY 4.0**. Anyone may copy, adapt,
redistribute and use them commercially, **provided the author and the source
are credited.** For example:

```
Source: JJOME (jihodaya), "JMPlayer format notes", 2026.
        https://github.com/jihodaya/jmplayer
```

If you carry any of this into code, please leave that line in a source comment.

## Documents

| File | Format | Original program |
|---|---|---|
| [gyb.md](gyb.md) | `.GYB` Gayobang karaoke | GAYOBANG.EXE (DOS) |
| [oka-okm-okw.md](oka-okm-okw.md) | `.OKA` `.OKM` `.OKW` Oksori karaoke | NORE45.EXE (DOS) |
| [opl-instrument.md](opl-instrument.md) | The instrument record shared by GYB and OKA, `.BNK` banks, built-in instruments | GAYOBANG / NORE45 |
| [nob.md](nob.md) | `.NOB` Oksori karaoke (before 4.0) | Oksori karaoke |
| [iss.md](iss.md) | `.ISS` IMS lyric file | Hanulsori IMS |
| [rcp.md](rcp.md) | `.RCP` Recomposer + `.GSD` setup | Recomposer (PC-98 / X68000) |
| [sng-ballade.md](sng-ballade.md) | `.SNG` Ballade | Ballade / ミュージくん / ミュージ郎 (PC-98) |
| [mdx.md](mdx.md) | `.MDX` + `.PDX` | MXDRV (X68000) |
| [mdz-mld.md](mdz-mld.md) | `.MDZ` MLD | MLD (X68000) |

## Marks

Each item says how certain it is.

| Mark | Meaning |
|---|---|
| ✅ | **Confirmed** — checked against the original program's code, output the original produced, or a recording of real hardware |
| 📏 | **Fitted by measurement** — the reason is not known, but the value was set by measuring many files. May change if better evidence turns up |
| ❓ | **Unknown** — not yet worked out. Marked separately so that guesses are never presented as fact |

Byte order is **little-endian** (x86) unless stated otherwise. The X68000
formats (MDX, MDZ) are **big-endian** (68000). Offsets are hexadecimal.

## How it was worked out — the answer keys

Guessing is easy to get wrong. Most of these notes were settled by **comparing
against an answer the original produced.** The answer key used for each format
is below; it is probably the most useful part for anyone repeating the work.

| Format | Answer key | Method |
|---|---|---|
| GYB / OKA | GAYOBANG.EXE · NORE45.EXE themselves | Decompiled with Ghidra to read the OPL driver. Our renders compared octave band by octave band against recordings made with the original program in DOSBox |
| NOB | 574 songs from the library | Regression between lyric position and the melody channel's note times |
| RCP | **303 songs** each shipped with a reference `.MID` | Conversion compared channel by channel, note by note |
| SNG | **SNG2S 3.3** (M. Saito, 1993) | Run on 64-bit Windows under msdos-player. Fields found by patching part of a real file and watching what came out |
| MDX | **mxwav** (shipped with MXDRVg) | 101 songs compared against renders by the original driver. Writing one- or two-note MDX files by hand and rendering them was the most effective method of all |
| MDZ | **mlc.x · mdz2mus.x** (MLD247) | Run on Windows under run68x. MML compiled and the bytes read; tick counts compared against the per-track counts the compiler reports |

One lesson: **do not trust a value fitted to one song.** Most of the things in
these notes that were wrong and later corrected looked right on one or two
songs and turned out wrong across hundreds.
