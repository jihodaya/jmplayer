# License Information for JJoMe MIDI Player

This document contains license information for the JJoMe MIDI Player project and its third-party dependencies.

## JJoMe MIDI Player Project License

The source code for the JJoMe MIDI Player project is released under the **MIT License** (see the [`LICENSE`](LICENSE) file for the full text). You are free to use, modify, copy, distribute, and sell this code and its compiled binaries, provided the copyright notice and permission notice are kept in copies.

There is no warranty for this software. Use at your own risk.

## Format Notes (`docs/formats/`)

The file-format notes in [`docs/formats/`](docs/formats/README.md) are licensed
separately under **Creative Commons Attribution 4.0 International (CC BY 4.0)**
— <https://creativecommons.org/licenses/by/4.0/>. You may copy, adapt and share
them for any purpose, including commercially, provided you credit the author
and link to the source:

```
JJOME (jihodaya), "JMPlayer format notes", 2026. https://github.com/jihodaya/jmplayer
```

## Dependency Licenses

JJoMe MIDI Player is developed using the following libraries and resources:

### Qt 6

*   **License:** GNU Lesser General Public License, Version 3 (LGPLv3)
*   **Copyright:** Copyright (C) 2023 The Qt Company Ltd. and other contributors.
*   **Website:** [https://www.qt.io](https://www.qt.io)
*   **Source Code:** The source code for Qt is available for download at [https://www.qt.io/download-open-source](https://www.qt.io/download-open-source).

**Notice of Obligations under LGPLv3**

This application is built using the Qt toolkit, which is licensed under the GNU Lesser General Public License, Version 3. In compliance with the LGPLv3, we provide the following notices and rights:

1.  **Right to Modification and Reverse Engineering:** You are granted the right to modify the Qt libraries and to reverse engineer the MidiPlayer application for the purpose of debugging such modifications. As this application is dynamically linked against the Qt libraries (DLLs), you may replace them with your own or modified versions.

2.  **License and Source Code Availability:** A copy of the LGPLv3 must be distributed with this application. The full text of the license is available at [https://www.gnu.org/licenses/lgpl-3.0.txt](https://www.gnu.org/licenses/lgpl-3.0.txt). The corresponding source code for the Qt libraries used can be obtained from the official Qt website linked above.

### AdPlug

*   **License:** GNU Lesser General Public License, Version 2.1 (LGPLv2.1)
*   **Copyright:** Copyright (C) 1999 - 2023 Simon Peter and others
*   **Website:** [https://github.com/adplug/adplug](https://github.com/adplug/adplug)
*   **Notice:** AdPlug is used to provide playback support for OPL and AdLib Tracker II formats. The library is dynamically linked or statically linked under the provisions of the LGPL.

### libbinio

*   **License:** GNU Lesser General Public License, Version 2.1 (LGPLv2.1)
*   **Copyright:** Copyright (C) 2002 - 2023 Simon Peter
*   **Website:** [https://github.com/adplug/libbinio](https://github.com/adplug/libbinio)
*   **Notice:** libbinio is a dependency of AdPlug, providing binary stream I/O.

### TinySoundFont (tsf)

*   **License:** MIT License
*   **Copyright:** Copyright (C) 2017-2023 Bernhard Schellkoopf
*   **Website:** [https://github.com/schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont)

### miniaudio

*   **License:** MIT No Attribution (MIT-0) / Public Domain
*   **Copyright:** Copyright (C) 2023 David Reid
*   **Website:** [https://github.com/mackron/miniaudio](https://github.com/mackron/miniaudio)

### Windows Platform Libraries

*   **Libraries:** `winmm.lib`, `dwmapi.lib`, etc.
*   **License:** These are system libraries that are part of the Microsoft Windows operating system. Their use is governed by the Windows End User License Agreement (EULA).

### Included SoundFonts

*   **GeneralUser GS:**
    *   **Website:** [http://schristiancollins.com/generaluser.php](http://schristiancollins.com/generaluser.php)
    *   **License:** Free for personal and commercial use with attribution. See the documentation accompanying the SoundFont for detailed licensing terms.
*   **VintageDreamsWaves-v2:**
    *   **License:** Released into the Public Domain or free for use. See the documentation accompanying the SoundFont for specific details.

### Legacy OPL Instrument Bank & Sample Music (BK)

*   **STANDARD.BNK:**
    *   **Description:** The default OPL FM instrument bank file compatible with Hanulso's IMS player format.
    *   **Notice:** Distributed solely for legacy compatibility, non-commercial archiving, and educational research of OPL sound synthesis.
*   **Sample Songs (BK/ folder):**
    *   **Description:** Legacy IMS, ISS, and ROL songs used in 1990s Korean PC music players (Oksori, Hanulso, etc.).
    *   **Notice:** Provided for non-commercial archiving and format testing purposes. All copyrights of the original compositions belong to their respective authors. Special thanks to **BK (병코돌고래)** for providing these sample files.

### munt (libmt32emu) - MT-32 / CM-32L emulation

*   **License:** GNU Lesser General Public License, Version 2.1 or later (LGPLv2.1+)
*   **Copyright:** Copyright (C) 2003-2026 Dean Beeler, Jerome Fisher, Sergey V. Mikayev and others
*   **Website:** [https://github.com/munt/munt](https://github.com/munt/munt)
*   **Notice:** Built as a separate library and linked dynamically (`libmt32emu-2.dll`), so it can be replaced. Roland's MT-32 ROM images are not included.

### Nuked-OPM - YM2151 emulation (`.mdx`, `.mdz`, `.vgm`)

*   **License:** GNU Lesser General Public License, Version 2.1 or later (LGPLv2.1+)
*   **Copyright:** Copyright (C) 2020, 2026 Nuke.YKT
*   **Website:** [https://github.com/nukeykt/Nuked-OPM](https://github.com/nukeykt/Nuked-OPM)
*   **Notice:** Compiled into the program (`mdxcore/opm.c`). The complete source of this program is published, so it can be rebuilt against a modified copy.

### Nuked-OPN2 - YM2612 / YM3438 emulation (`.vgm`)

*   **License:** GNU Lesser General Public License, Version 2.1 or later (LGPLv2.1+)
*   **Copyright:** Copyright (C) 2017-2022 Alexey Khokholov (Nuke.YKT)
*   **Website:** [https://github.com/nukeykt/Nuked-OPN2](https://github.com/nukeykt/Nuked-OPN2)
*   **Notice:** Compiled into the program (`vgmcore/ym3438.c`), on the same terms as Nuked-OPM above.

### emu2413 - YM2413 emulation (`.vgm`)

*   **License:** MIT License
*   **Copyright:** Copyright (c) 2001-2019 Mitsutaka Okazaki
*   **Website:** [https://github.com/digital-sound-antiques/emu2413](https://github.com/digital-sound-antiques/emu2413)

### emu2149 - AY-3-8910 / YM2149 emulation (`.vgm`)

*   **License:** MIT License
*   **Copyright:** Copyright (c) 2014 Mitsutaka Okazaki
*   **Website:** [https://github.com/digital-sound-antiques/emu2149](https://github.com/digital-sound-antiques/emu2149)

The MIT License text that applies to both of the above:

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ymfm - YM2610 emulation (`.vgm`)

*   **License:** BSD 3-Clause License
*   **Copyright:** Copyright (c) 2021, Aaron Giles
*   **Website:** [https://github.com/aaronsgiles/ymfm](https://github.com/aaronsgiles/ymfm)

### qsound-hle - Capcom QSound emulation (`.vgm`)

*   **License:** BSD 3-Clause License
*   **Copyright:** Copyright (c) 2018, ValleyBell, Ian Karlsson
*   **Website:** [https://github.com/ValleyBell/qsound-hle](https://github.com/ValleyBell/qsound-hle)
*   **Modification:** one line in `get_sample()` applies the ROM address mask, so a song cannot read outside its own sample data. The change is marked in the source.

The BSD 3-Clause License text that applies to both of the above:

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
