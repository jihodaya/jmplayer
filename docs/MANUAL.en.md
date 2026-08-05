# JMPlayer R2.4e — User Manual

*[한국어 매뉴얼은 여기 → MANUAL.ko.md](MANUAL.ko.md)*

JMPlayer is a retro music player for Windows that plays standard MIDI files and 1990s Korean DOS-era music/karaoke formats, using SoundFont synthesis and software OPL3 (AdLib) FM emulation.

---

## 1. Supported Formats

| Extension | Description | Sound engine | Lyrics |
|---|---|---|---|
| `.mid` `.midi` | Standard MIDI | SoundFont (JJoMe Synth) or system MIDI device | – |
| `.nob` | Oksori karaoke (MIDI + Johab lyrics) | SoundFont / MIDI device | ✔ syllable sync |
| `.oka` | Oksori karaoke (OPL) | OPL3 emulation | ✔ syllable sync |
| `.okm` `.okw` | Oksori karaoke (MIDI) | SoundFont / MIDI device | ✔ syllable sync |
| `.gyb` | Gayobang karaoke (OPL) — Korean & English releases | OPL3 emulation | ✔ syllable sync |
| `.ims` (+`.iss`) | IMS AdLib music (+ISS lyric file) | OPL3 emulation | ✔ (with .iss) |
| `.rol` | AdLib Visual Composer | OPL3 emulation | – |
| `.sop` | Note (sopepos) | OPL3 emulation | – |
| `.vgm` `.vgz` | VGM chiptune logs (OPL) | OPL3 emulation | – |
| `.zip` | Archive containing any of the above | (auto-extracted) | – |

## 2. Main Window

### Top bar
* **Output device combo** — choose `[JJoMe Synth (SoundFont)]` (built-in synthesizer) or any system MIDI output device. Applies to MIDI/NOB/OKM playback; OPL formats always use the built-in OPL3 emulator.
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
| **Ctrl+R** | Start / stop WAV recording |

## 4. Karaoke Lyrics

* The **Lyrics Window** (📜) shows decoded lyrics for NOB / OKA / OKM / GYB (and IMS with an `.iss` file).
* Highlighting follows the singing **syllable by syllable**; both Korean (Johab) and English (export GYB) lyrics are supported. Line changes glide smoothly — the first syllable of the next line fades in.
* **NOB channel selector** — if the sync marker channel is unusual, pick the marker channel in the lyrics window.
* **Edit Lyrics** — built-in editor with timing symbols:
  * `~` hold the previous syllable one more beat
  * `#` insert a one-beat rest before the next syllable
  * `@` repeat from the beginning after this line
  * **Export** saves the lyrics to a text file.

## 5. Channel Monitor (📊)

* **MIDI mode** — 16 channels in an OPL-style dark theme: per-channel gradient VU bar with peak hold, instrument-name card, program number and the currently played notes (colored by velocity). The sound module type (**GM / MT-32 / GS / XG**) is auto-detected and shown with a confidence score.
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

## 11. Tips

* All settings (volume, device, repeat mode, DSP level, last folder/position) are saved automatically.
* ZIP archives can be opened directly — contained songs are listed and playable.
* If an OPL song sounds wrong, try the bundled `STANDARD.BNK` (BNK button) and DSP off first.
