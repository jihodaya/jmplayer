# JMPlayer R2.7b — User Manual

*[한국어 매뉴얼은 여기 → MANUAL.ko.md](MANUAL.ko.md)*

JMPlayer is a retro music player for Windows that plays standard MIDI files and 1990s Korean DOS-era music/karaoke formats, using SoundFont synthesis and software OPL3 (AdLib) FM emulation.

---

## 1. Supported Formats

| Extension | Description | Sound engine | Lyrics |
|---|---|---|---|
| `.mid` `.midi` | Standard MIDI | SoundFont (JJoMe Synth) or system MIDI device | – |
| `.nob` | Oksori karaoke (MIDI + Johab lyrics) | SoundFont / MIDI device | ✔ syllable sync |
| `.oka` | Oksori karaoke (OPL) | OPL3 emulation **or a MIDI module** (section 14) | ✔ syllable sync |
| `.okm` `.okw` | Oksori karaoke (MIDI) | SoundFont / MIDI device | ✔ syllable sync |
| `.gyb` | Gayobang karaoke (OPL) — Korean & English releases | OPL3 emulation **or a MIDI module** (section 14) | ✔ syllable sync |
| `.ims` (+`.iss`) | IMS AdLib music (+ISS lyric file) | OPL3 emulation | ✔ (with .iss) |
| `.rol` | AdLib Visual Composer | OPL3 emulation | – |
| `.sop` | Note (sopepos) | OPL3 emulation | – |
| `.vgm` `.vgz` | VGM chiptune logs (OPL) | OPL3 emulation | – |
| `.zip` | Archive containing any of the above | (auto-extracted) | – |

## 2. Main Window

### Top bar
* **Output device combo** — choose `[JJoMe Synth (SoundFont)]` (built-in synthesizer), any system MIDI output device, or **Nuked SC-55** (section 11). Applies to MIDI/NOB/OKM playback; OPL formats always use the built-in OPL3 emulator.
* **R** — refresh the list of MIDI devices.
* **⚙️** — open the **SoundFont Manager** (switch the active `.sf2`).
* **Search box** — live filter for the playlist; clearing the text restores the full list.
* **F1** — open the keyboard shortcut / feature help dialog.

### Playlist
* Folder-tree style: **Enter** or double-click a folder to enter it, `..` to go up.
* **File** / **Folder** — add individual files or scan a whole folder (subfolders included).
* **Remove** — remove the selected entry, **Sort** — sort entries, **Playlist** — playlist management.
* **L** — reload/refresh the playlist.
* **Delete** key — permanently remove the selected song.
* Titles for NOB/OKA/GYB files are decoded from the embedded Johab/EUC-KR data automatically.
* The last played file and position are restored on the next start.

### Transport
* **REW / FW** — seek backward / forward 5 seconds (same as ← / → keys).
* **PREV / NEXT** — previous / next song relative to the *currently playing* file (the view selection is not disturbed).
* **PLAY / STOP**, progress slider (drag to seek), volume slider.
* **Repeat-mode button** — cycles: play once → repeat current song → repeat all → shuffle.

### Time / status display
```
0008/0026 00:05 03:18  561 | Key: 0 | 132 - 100%
 track/total cur  total ticks  transpose  BPM  tempo
```

### Right-side tool buttons
| Button | Function |
|---|---|
| 📊 | Toggle the **Channel Monitor** window |
| **DSP** | Analog simulation for OPL output (LPF + soft saturation), 3 levels: DSP → DSP2 → DSP3 → off |
| 🎹 | Toggle the **Piano Roll** window |
| **BNK** | Select an external OPL instrument bank (`.BNK` / `.IBK`) for AdLib formats |
| **MIDI** | play a `.gyb` / `.oka` **through a MIDI module** instead of OPL (section 14). Appears when one is selected |
| **OUT** | **OPL tunnel** — stream OPL registers to the selected MIDI device (section 12, `Ctrl+Shift+O`) |
| 📜 | Toggle the **Lyrics Window** |
| ⏺ | Record the audio output to a WAV file (also **Ctrl+R**) |

## 3. Keyboard Shortcuts

| Key | Function |
|---|---|
| **F1** | Help dialog |
| **Space** | Pause / resume the current song |
| **Enter** | Enter selected folder / play selected song |
| **Delete** | Remove selected song from the playlist |
| **← / →** | Seek −5 s / +5 s |
| **↑ / ↓** | Move through the playlist (works while playing) |
| **+ / −** | Master volume ±5 % |
| **F7 / F8** | Tempo −5 % / +5 % (range 50–150 %) |
| **F9 / F10** | Key transpose −1 / +1 semitone (range −6…+6, all formats) |
| **F11** | Reset tempo and key to the original |
| **F12** | OPL pseudo-stereo / performance mode dialog (modes 1–9) |
| **F5** | **Change instruments** — for a `.gyb` / `.oka` played through MIDI (section 14) |
| **F6** | **Sound-module reset** — whether to reset the device on each new song (section 13) |
| **Ctrl+R** | Start / stop WAV recording |
| **Ctrl+Shift+O** | Toggle the OPL tunnel (same as the **OUT** button) |
| **Esc** | Clear the search box and return to the list |

## 4. Karaoke Lyrics

* The **Lyrics Window** (📜) shows decoded lyrics for NOB / OKA / OKM / GYB (and IMS with an `.iss` file).
* Highlighting follows the singing **syllable by syllable**; both Korean (Johab) and English (export GYB) lyrics are supported. Line changes glide smoothly — the first syllable of the next line fades in.
* **NOB channel selector** — if the sync marker channel is unusual, pick the marker channel in the lyrics window.
* **Edit Lyrics** — built-in editor with timing symbols:
  * `~` hold the previous syllable one more beat
  * `#` insert a one-beat rest before the next syllable
  * `@` repeat from the beginning after this line
* **Editing never touches the original file.** The result is saved as a `.txt` of the same name beside the song, and is used in preference from then on. The `.NOB` and friends are left intact, so deleting the `.txt` restores the original lyrics.

## 5. Channel Monitor (📊)

* **MIDI mode** — 16 channels in an OPL-style dark theme: per-channel gradient VU bar with peak hold, instrument-name card, program number and the currently played notes (colored by velocity). The sound module type (**GM / MT-32 / GS / XG**) is identified from the reset messages, vendor SysEx and text the file actually contains, and shown with the evidence used. A file that declares nothing reads **`GM (assumed)`** in grey - it is being read as GM, but that is distinct from a file which really does declare GM.
* **OPL mode** (IMS/ROL/SOP/GYB/OKA/VGM) — 20 FM voices with instrument cards, note/velocity readouts and the same VU meters; voice instrument names follow the actual OPL patches in real time.

## 6. Piano Roll (🎹)

A scrolling note view of what is currently playing. Works with both MIDI and OPL playback.

## 7. OPL Sound Options

* **DSP** — three levels of "analog warmth" (low-pass filter + soft saturation) applied to OPL output.
* **F12 stereo modes** — nine pseudo-stereo voice-panning layouts for OPL playback (mode 1 = original mono).
* **BNK** — load an external AdLib instrument bank (`.BNK`, `.IBK`); useful for IMS/ROL files that shipped with their own banks. `STANDARD.BNK` is bundled as the default.

## 8. Recording (⏺ / Ctrl+R)

Records whatever is playing (any format) into a **WAV** file. Press again to stop. Recording runs in a lock-free buffer, so playback is not disturbed.

## 9. SoundFont Manager (⚙️)

Switch the active `.sf2` SoundFont used by the built-in synthesizer. Two SoundFonts are bundled: *GeneralUser GS* and *VintageDreamsWaves*.

## 10. Settings & Data Locations

JMPlayer stores its data in your **Documents** folder, under `JMPLAYER`:

| File | Contents |
|---|---|
| `Documents\JMPLAYER\settings.ini` | All settings — volume, output device, repeat mode, DSP level, last played file/position, window options |
| `Documents\JMPLAYER\playlist.json` | The playlist tree (folders and songs you added) |

The folder is created when there is first something to save. To fully reset the player, delete these two files; to move your setup to another PC, copy them.

### Portable mode - running from a USB stick

Create a folder named **`cfg`** next to the executable and the player keeps its settings and playlist there instead, leaving your Documents folder completely untouched.

```
E:\JMPlayer\
    JMPlayer_R2.5h.exe
    cfg\        <- create this yourself; its presence turns portable mode on
    Music\      <- created for you; put your songs here
    BK\         <- bundled songs
```

* Nothing creates `cfg` for you. **You creating it is the signal**, so an ordinary install is never affected.
* Delete `cfg` and the next run goes back to Documents.
* Keep your songs in **`Music`**. The list then survives the stick being `E:` on one machine and `F:` on the next. That folder is rescanned at every launch, so files you drop in appear the next time you start the player.
* Songs kept outside `Music` still follow along as long as they are **on the same drive**. Anything on another drive is stored with its full path, as before.
* The location in use is shown at the bottom of the **`F1`** help.
* Note: everything the player writes stays inside its own folder, but Windows keeps its own records (recent-documents lists and so on) that no application can suppress.

## 11. Nuked SC-55 (optional)

Songs can be played through Nuked SC-55, a cycle-accurate emulator of the Roland
SC-55, by selecting **Nuked SC-55** in the output device combo.

**It is not bundled** - licensing does not allow it to be redistributed, so you
supply it yourself:

1. Build the patched emulator following the notes in `emulator-patch\`.
2. Put the resulting executable in the `NukedSC55\` folder.
3. Put the SC-55 ROM files in the same folder. **The ROMs belong to Roland and are
   not provided.**

Once that is in place the emulator starts alongside the first song, and playback
waits for it to finish booting. No virtual MIDI port such as loopMIDI is needed.

## 12. OPL Tunnel (**OUT**)

While an OPL format plays, this streams the register writes out to the **selected
MIDI output device**, so real or emulated OPL hardware - a Raspberry Pi jukebox,
for instance - plays the same performance.

It is of no use without a device on the other end. Toggle it with the **OUT**
button or `Ctrl+Shift+O`; the setting is remembered between runs.

## 13. Sound-Module Reset (**F6**)

Chooses whether a reset is sent to the output device before each new song, so the
previous song leaves no patches, volumes or effects behind.

* Pick which resets to send - **GM System On**, **GS Reset** (Roland), **XG System
  On** (Yamaha), **MT-32 Reset**.
* A **delay in milliseconds** can follow the reset. Real hardware takes a moment to
  act on one, and starting the music too soon swallows the first notes.
* **Off by default.** The built-in SoundFont does not need it; turn it on when
  driving an external MIDI module.


## 14. `.GYB` / `.OKA` through a MIDI module (**MIDI** button, **F5**)

GAYOBANG and NORE45 could both play these songs on a **MIDI module instead of the OPL chip** (sound source 7). This brings that back.

**Turning it on** — select a `.gyb` or `.oka` in the playlist and the **MIDI** button appears. Press it and that song plays through whatever MIDI output is selected — SoundFont, a system MIDI device, or Nuked SC-55. You can set it before starting the song.

**Why instruments have to be assigned** — a program change in these formats indexes the song's **own OPL instrument table**, not General MIDI. Sent as-is, a bass drum lands on Vibraphone. So each slot needs a GM instrument.

**F5 — the instrument list**

| Column | Meaning |
|---|---|
| OPL instrument | the name the song gives that slot |
| Notes | how many notes it actually plays (sorted, busiest first) |
| Default from | one of the three below |
| Type | melodic or percussion |
| Play as | one of the 128 GM instruments, or a percussion note |

Defaults are ranked by how much they can be trusted:

1. **From the song file** — somebody chose it in the DOS program in the 1990s and it was saved into the file. Stored as an MT-32 tone number, shown translated to GM. The most reliable of the three.
2. **Matched by name** — the patch name is matched against a rule set. Across the real library this settles about **89 %** of slot uses.
3. **Not recognised** — nothing is known. **These are the ones that need your ear, and there are usually only three or four per song.** Tick *Show only what was not recognised* to see just those.

**Changes are heard as you make them** — pick an instrument while the song plays and it changes at once. (Switching a row between melodic and percussion moves it to another channel, so only that reloads the song.) A changed row is highlighted, and **Revert all** puts every one of them back.

**Nothing is written until you press Save.** Try things out, close the window, and no file is left behind. **Save** writes a small `SONGNAME.GYB.ini` beside the song, and the next time you open that song it comes back. The original song file is never modified.

> Songs in a location that cannot be written to — a CD, for instance — save into the settings folder instead.

## 15. Tips

* All settings (volume, device, repeat mode, DSP level, last folder/position) are saved automatically.
* ZIP archives can be opened directly — contained songs are listed and playable.
* If an OPL song sounds wrong, try the bundled `STANDARD.BNK` (BNK button) and DSP off first.
