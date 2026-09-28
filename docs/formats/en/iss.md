# `.ISS` — IMS lyric file

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../iss.md)

A **karaoke lyric file** paired by name with a Hanulsori IMS music file
(`.IMS`). `.IMS` itself is a public format AdPlug already plays, so it is not
covered here.

## 1. Layout

```
0x00   20  header string
0x14   10  reserved
0x1E   30  lyricist (Johab)
0x3C   30  composer
0x5A   30  singer
0x78   30  editor
0x96    2  record count R
0x98    2  line count L
0x9A       sync records × R   (5 bytes each)
           lyric lines × L    (64 bytes each, Johab, NUL-terminated)
```

✅ **Sync record (5 bytes)**

```
u16  lyric tick (IMS tick ÷ multiplier)
u8   line number (from 0)
u8   byte position in the line where colouring starts
u8   number of bytes to colour
```

Reading the records in order, a new line is shown whenever the line number
changes. A repeated chorus simply brings the same line number back.

## 2. Timing

```
milliseconds = lyric tick × multiplier × 60000 / (basicTempo × tickBeat)
```

`basicTempo` and `tickBeat` come from the header of the paired `.IMS`.

📏 **The multiplier is 8 if the lyrics contain Korean, 10 if not.** Both values
were fitted on real songs; ❓ why it depends on the language is not known.

## 3. Caution

- Some 2-byte codes cannot be converted as Johab (glyphs from DOS-only fonts).
  If Windows code page 1361 turns one into `?` or into the private-use area
  (U+E000-F8FF), skip it. A **byte-position-to-character map** has to be kept
  alongside, or the colouring ranges drift.
