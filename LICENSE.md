# License Information for JJoMe MIDI Player

This document contains license information for the JJoMe MIDI Player project and its third-party dependencies.

## JJoMe MIDI Player Project License

The source code for the JJoMe MIDI Player project is released under the **MIT License** (see the [`LICENSE`](LICENSE) file for the full text). You are free to use, modify, copy, distribute, and sell this code and its compiled binaries, provided the copyright notice and permission notice are kept in copies.

There is no warranty for this software. Use at your own risk.

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
