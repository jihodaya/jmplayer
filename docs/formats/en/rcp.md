# `.RCP` — Recomposer (PC-98 / X68000) + `.GSD` setup file

> **Author**: JJOME (GitHub [jihodaya](https://github.com/jihodaya)) — [JMPlayer](https://github.com/jihodaya/jmplayer) format notes
> **First written**: 2026-09-28 · **Licence**: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) — please credit the author and the source when reusing.
> **Evidence**: **303** Recomposer songs each shipped with a reference `.MID` (supplied by a tester) plus 3 more, and a batch conversion of 213 songs in the library.
> Marks: ✅ confirmed · 📏 fitted by measurement · ❓ unknown ([README](README.md)) · [한국어 원문](../rcp.md)

The song file of **Recomposer**, a sequencer by COME ON MUSIC of Japan. Public
material on RCP already exists, but checked against 303 reference MIDI files
it does not hold up as is in many places. This document records **the
readings confirmed by that comparison.**

Result: of 303 songs, **225 match note for note on every channel**, 66 match
the reference's opening exactly with extra material only at the reference's
end, and 12 still differ. **Songs whose notes are all correct: 291 / 303 (96%).**

> `.R36` / `.G36` / `.G18` (Recomposer's other forms) were never checked — no
> sample files exist here. This document covers `.RCP` only.

---

## 1. Song header

| Offset | Size | Contents | |
|---|---|---|---|
| `0x000` | 32 | `"RCM-PC98V2.0(C)COME ON MUSIC"` | ✅ |
| `0x020` | 64 | Song title (**Shift-JIS**) | ✅ |
| `0x1C0` | 1 | Resolution (ticks per quarter note, usually 48) | ✅ |
| `0x1C1` | 1 | Base tempo (BPM) | ✅ |
| `0x1C2` | 1 | Time-signature numerator | ✅ |
| `0x1C3` | 1 | Time-signature denominator | ✅ |
| `0x1C5` | 1 | **Song-wide transpose, signed** (`0xF4` = −12) | ✅ |
| `0x1C6` | 16 | CM-64/MT-32 setup file name (`.CM6`) — `.CM6` contents not yet read | ❓ |
| `0x1D6` | 16 | **SC-55 setup file name (`.GSD`)** | ✅ |
| `0x406` | 8 × 48 | Eight user SysEx messages (start `F0`, end `F7`) | ✅ |
| `0x586` | … | Start of track data | ✅ |

✅ **Read the title as Shift-JIS only.** A decoder that tries Korean first turns
full-width Latin (`Ｂａｍ`) into perfectly valid, unrelated Korean syllables.

✅ `0x1C5` song transpose: **all 200 songs that already matched carry 0.**
`CROSS_4` carries −12 and `LOVE` +2; ignoring it shifts every melodic channel
by that much.

## 2. Track header — 44 bytes (not 48)

| Offset | Size | Contents | |
|---|---|---|---|
| `+0` | 2 | Track size (header included) | ✅ |
| `+2` | 1 | Track number | ✅ |
| `+3` | 1 | Rhythm flag | ✅ |
| `+4` | 1 | MIDI channel (0-15) | ✅ |
| `+5` | 1 | **Track transpose** (below) | ✅ |
| `+6` | 1 | **Delays the track start by a few ticks** (signed) | ✅ |
| `+7` | 1 | Mode. **Bit 0 = mute** | ✅ |
| `+8` | 36 | Track name | ✅ |

✅ **The header is 44 bytes.** Reading 48 throws away the first event of every
track. What that costs depends on the event, which made it hard to find: one
track of `BAMBOO.RCP` lost a 192-tick rest, its echo track a 200-tick one.

✅ **Track transpose `+5`**: **bit 7 set means "no transpose"** — the whole bit,
not only the value `0x80`. 64-127 are −64 to −1 (116 = −12). Computing
`>= 64 ? −128` gets `0x80` right by luck but puts 140 (`0x8C`) twelve
semitones sharp and 129 one semitone sharp.

✅ **Track delay `+6`**: in a song carrying 3 on its drum track and 1-2 on four
others, the measured drift of each of its 13 channels matched this byte exactly.

✅ **Mute `+7` bit 0**: a track muted in the sequencer keeps its notes in the
file. The reference MIDI contains **none** of them. The other bits (2, 4, 32,
56) are not a mute — every track carrying them is in the reference.

## 3. Events — 4 bytes

```
[command, step, gate, velocity]
```

`step` is the number of ticks to the next event (how much time passes);
`gate` is the note length.

### Notes (`0x00`-`0x7F`)

The command is the note number. Transpose = track `+5` + song `0x1C5`.
**MIDI channel 10, and tracks whose name contains DRUM or PERC, are not
transposed.**

✅ **A gate longer than the step is a tie.** It joins the next note of the same
pitch without re-striking; the last note of the chain decides where it ends.
`3DAYS.RCP` writes ties as step 192 with gate 193 — exactly one tick over.

### Commands

| Command | Meaning | Time advances | |
|---|---|---|---|
| `0x80`-`0x87` | Send user SysEx 0-7 as is | step | ✅ |
| `0x90`-`0x97` | User SysEx 0-7, with each `0x80` in it replaced by gate and each `0x81` by velocity | step | ✅ |
| `0x98` | ❓ **Unknown.** Once treated like `0xFC`, which was wrong (below) | step | ❓ |
| `0xDD` | Top two bytes of a Roland address = gate, velocity | step | ✅ |
| `0xDE` | Last address byte = gate, value = velocity → **sends one GS parameter write** | step | ✅ |
| `0xDF` | Roland device number = gate, model number = velocity | step | ✅ |
| `0xE1` | Bank select (CC0 = velocity) | step | ✅ |
| `0xE2` | **Bank + program**: CC0 = velocity, CC32 = 0, program = gate. **Not a tempo** | step | ✅ |
| `0xE6` | **Channel change. The value is gate, counted from 1** (1-16) | step | ✅ |
| `0xE7` | **Tempo = song base tempo × gate / 64.** A ratio, not a BPM (64 = ×1) | step | ✅ |
| `0xEA` `0xEB` `0xD2` | Control change (number = gate, value = velocity) | step | ✅ |
| `0xEC` `0xD3` | Program change (program = gate, bank = velocity) | step | ✅ |
| `0xEE` `0xD0` | Pitch bend (LSB = gate, MSB = velocity) | step | ✅ |
| `0xF5` | Key signature. **No time passes** | none | ✅ |
| `0xF6` `0xF7` | Comment text. **All three bytes are characters; no time passes** | none | ✅ |
| `0xF8` | Loop end. step = **total number of passes** (not time) | none | ✅ |
| `0xF9` | Loop start (not time) | none | ✅ |
| `0xFC` | **Same-measure repeat** (section 4) | one measure | ✅ |
| `0xFD` | Measure end. **Its step is not time** | none | ✅ |
| `0xFE` | End of track | — | ✅ |

Every command that "takes no time" once caused trouble by adding its step.
`0xFD` pushed the music one tick late per bar (966 times in `BAMBOO`), `0xF5`
accumulated three ticks at a time, and one comment whose first character
happened to be `0x0C` put a whole drum track twelve ticks behind.

`0xDF`/`0xDD`/`0xDE` together make one
`F0 41 <device> <model> 12 <address ×3> <value> <checksum> F7`. Dropping them
loses 156 of `BAMBOO`'s 211 SysEx messages.

## 4. `0xFC` same-measure repeat — the hardest command

✅ `step` looks like a measure number, but **what is followed is the other two
bytes**: `gate | (velocity << 8)` = **a byte position counted from the start
of the track, 44-byte header included.** Looking the measure up by number
disagrees with the position in 81 of a drum track's 130 uses (only 14 of 81
agree).

✅ **A repeat met inside a repeat is a tail call.** It jumps without pushing
another return, so one `0xFC` always plays **exactly one bar** however long the
chain runs. Do not end a measure at "the next `0xFD`": one channel of
`6PONGI4` has fifty `0xFC` in a row with no `0xFD` among them (one per bar,
each naming a different earlier bar).

✅ **What ends a repeat is position.** An `0xFC` sitting at **exactly the byte
the repeat jumped to** means "this bar is a copy too" and is followed. One met
**further in** means the next bar has begun, and ends the repeat. This rule
took the 303-song corpus from 219 to 225 matching with nothing broken. Two
simpler rules (end at any `0xFC`; measure one bar's length from the time
signature) both lost songs.

✅ **Never take the same `0xFC` twice within one chain.** One file (`IKDT2_20`)
has a chain that closes on itself; followed without this guard a 15 KB file
becomes 574,168 note-ons (the reference has 3,989). A depth limit is not used,
because it reintroduces the same error at the limit.

### `0x98` is not `0xFC`

`0x98` was once handled like `0xFC`, with nothing to justify it. 169 of the
library's 213 songs contain it, and in `YUNO2488.RCP` its "positions" mostly
fall inside the header or off 4-byte alignment (every genuine `0xFC` position
is aligned). Two that passed by luck formed a cycle: **zero notes, and
30,000-60,000 control changes per track.** It now just lets `step` pass.
❓ A file that uses `0x98` and ships a reference `.MID` would identify it.

## 5. `.GSD` — the SC-55 setup file (half the sound)

A file of the name at song header `0x1D6` sits beside the song. Without it the
parts play the wrong patches at default volume and pan, and nothing declares
which channel is rhythm.

✅ The reference MIDI sends this file **twice over**: once as bulk dumps only an
SC-55 understands, and again as ordinary channel messages, so that other
modules get the settings too.

### 5-1. Part blocks (16, base `0x0035`, stride `0x7A`)

| File offset | Contents | Sent as |
|---|---|---|
| `+0x00` | Voice reserve | `40 01 10` (5-4 below) |
| `+0x01` | **Tone bank** | CC0 (CC32 always 0) |
| `+0x02` | Tone number | program change |
| `+0x03` | **Receive channel** — which channel this block configures | |
| `+0x16` | Rhythm part or not | `40 1n 15 vv` |
| `+0x1A` | Level | CC7 |
| `+0x1D` | Pan | CC10 |
| `+0x22` | Chorus send | CC93 |
| `+0x23` | Reverb send | CC91 |
| `+0x43` | Pitch-bend range (biased by 64) | RPN 0,0 |

✅ **The block order is not the MIDI channel.** `+0x03` decides the channel.
In `3DAYS.GSD` blocks 1 and 9 configure each other's channels. Indexing by
position gets 14 of 16 right and quietly swaps two.

✅ The tone bank is at `+0x01`. `+0x00` (voice reserve) is 0 for most parts, so
reading the wrong offset looked right. Brute-forcing all 122 offsets narrowed
it to exactly one.

### 5-2. File part block → SC-55 part block

A file block is 122 bytes; the SC-55's part area is **112 bytes**. The values
are the same but **their positions differ.** Copying the block straight in
puts 66-71 of every 128 bytes on the wrong parameter, and the SC-55 shows an
error and stays silent. Most of the block is fixed values (0x40, 0x00); the
positions taken from the file are:

| SC-55 | File | Contents |
|---|---|---|
| `00`-`01` | `01`-`02` | tone bank, tone number |
| `02`-`03` | fixed `FF FF` | |
| `04` | `03` | receive channel |
| `05` | `B0` if file `16`, else `81` | rhythm part or not |
| `06` | `17` | key shift |
| `07` | fixed `80` | |
| `08` | `1A` | level |
| `09` | `1D` | pan |
| `0A`-`0B` | `1B`-`1C` | |
| `0C` | `19` | |
| `0D` | `1F` | |
| `0E`-`17` | `22`-`2B` | chorus, reverb, **tone modify** |
| `26`-`27` | `20`-`21` | |
| `2D`-`36` | `3C`-`45` | scale tuning |

✅ Rebuilding all 32 part blocks of the two songs with a reference from the
`.GSD` alone with this table gives **32 of 32 byte-identical.**

The bulk-dump address step is `0x90 + 0xE0 × slot`; `0xE0` is 224 nibbles =
112 bytes. The reference's message sizes (137 and 105 bytes) agree.

### 5-3. Drum maps

✅ A map is **83 notes starting at note 27**, four bytes per note. Map 1 starts
at `0x07D6`, map 2 at `0x0922`. Each of the four columns goes out as **two
messages** (notes 0-63 and 64-127) at blocks `02/03`, `06/07`, `08/09`,
`0A/0B`; the second map adds `0x10`. The first value of the second message of
each pair is **note 64**, not note 0 (`0x086A` = `0x07D6 + 37 × 4`). Missing
that looks like "one value off in one place". Rebuilding the 16 blocks gives
32/32.

Without the drum maps, every per-note drum level (79-127) falls back to the
default and **the whole kit comes out too strong.**

### 5-4. Voice reserve

✅ `40 01 10` shares the 24 voices out between the parts. Take each part
block's `+0x00` **in file order, with entries 0 and 10 swapped.** Trying 122
offsets × 3 orderings on both songs leaves exactly this one answer. Without
the swap, `3DAYS`'s drum part gets **no** voices at all.

## 6. Conventions of the JMPlayer converter (our choices, not the format)

- A **576-tick lead-in at 120 BPM** carries the setup, and the song's own
  tempo takes over on the tick the music starts. The reference MIDI does this.
  Songs without a `.GSD` skip the lead-in.
- A **GS reset goes out first**, which the reference MIDI lacks. The reference
  was recorded from an already-reset module; in practice the previous song's
  settings may still be there.
- Setup messages are spaced by their **MIDI wire time** (`1 + size/32` ticks).
  Sent all at once, a real SC-55 cannot keep up.
- **Events sharing a tick keep the order the file wrote them (stable sort).**
  `3DAYS` layers two sounds on one channel within a tick: note on → program
  change → note on. Only note-offs are moved forward.

## 7. Still unknown

- ❓ The `0x98` command.
- ❓ Bits of track header `+7` other than bit 0 (2, 4, 32, 56).
- ❓ Loop end `0xF8` with step = 0 (no pass count). The reference converter
  then appends **four more bars** of the loop at the end of the song (measured
  on the AK68GS family; most of the 66 "extra material at the end" songs above
  are this case). Taken to be that converter's convention rather than data in
  the file, and not copied.
- ❓ The 12 songs still differing (`GOKU2` +5,403 notes, `BARE1_1` +2,697,
  `FZ_PORTX` −893, and others).
- ⚠️ A reference `.MID` can be wrong too. `EM_OP`'s drum track has 400 notes
  after tick 13,636 plainly in the file that the reference drops. Before
  deciding a mismatch is ours, look at what the file actually contains.
