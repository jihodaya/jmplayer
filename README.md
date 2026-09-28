# 🚀 JMPlayer (JJoMe MIDI-OPL Player)
<img width="1372" height="675" alt="image" src="https://github.com/user-attachments/assets/24fafd45-3aa8-4a48-8774-9296c70173a9" />
<img width="520" height="293" alt="JMPLAYER_R24b" src="https://github.com/user-attachments/assets/bf4209e2-bff3-4e56-b502-bb252f41726e" />


https://www.youtube.com/@jjome_Plus

**JMPlayer** is a Windows player for retro computer music: 1990s Korean karaoke
and AdLib formats, Japanese PC-98 and Sharp X68000 sequencer files, VGM
chiptunes and standard MIDI. It plays each through the sound source it was
written for - an emulated OPL3, YM2151 or other chip, a built-in MT-32, the
Nuked-SC55 emulator, a SoundFont, or a real MIDI device - and shows karaoke
lyrics where the format carries them.

[한국어 설명은 아래에 있습니다 ↓](#한국어)

## ⬇️ Download

Get the latest build from **[Releases](https://github.com/jihodaya/jmplayer/releases)**
(Windows 10 / 11, 64-bit). Unzip and run - nothing is installed. Use the
`_eng` package for the English interface.

MT-32 ROMs and the Nuked-SC55 emulator are **not** included; see the manual
for where to put your own.

📖 **User manual:** [English](docs/MANUAL.en.md) · [한국어](docs/MANUAL.ko.md)

🔍 **Format notes:** [`docs/formats/`](docs/formats/README.md) — what was worked out
about each file format (GYB, OKA/OKM/OKW, NOB, ISS, RCP, Ballade SNG, MDX, MLD MDZ),
mostly by decompiling the original programs and checking against their own output.
Written in Korean, under CC BY 4.0.

## 🎵 Supported formats

| Extension | What it is | Played through |
|---|---|---|
| `.mid` `.midi` | Standard MIDI | SoundFont, MIDI device, Nuked-SC55, built-in MT-32 |
| `.nob` `.okm` `.okw` | Oksori karaoke (MIDI + lyrics) | as above |
| `.oka` `.gyb` | Oksori / Gayobang karaoke (OPL + lyrics) | OPL3 emulation, or MIDI |
| `.ims` (+`.iss`) `.rol` `.sop` | AdLib music (IMS lyrics in `.iss`) | OPL3 emulation (AdPlug) |
| `.rcp` | Recomposer (PC-98 / X68000), with its `.GSD` setup | MIDI sources, as `.mid` |
| `.sng` | Ballade / ミュージくん / ミュージ郎 (PC-98) | MIDI sources, as `.mid` |
| `.mdx` | Sharp X68000 MXDRV, with `.PDX` sample banks | YM2151 + ADPCM |
| `.mdz` | Sharp X68000 MLD | YM2151 + ADPCM and/or MIDI, together |
| `.vgm` `.vgz` | VGM chip logs | OPL2/OPL3, YM2612, YM2151, SN76489, Game Boy, YM2413, AY-3-8910, SegaPCM, YM2610, QSound |
| `.zip` | any of the above, zipped | unpacked automatically |

## 🌟 Features

* **Karaoke lyrics** with syllable-level highlighting (Johab / EUC-KR), in a separate window.
* **Built-in Roland MT-32 / CM-32L** (munt), with its twenty-character display. Bring your own ROMs.
* **Nuked-SC55** through a named pipe - no virtual MIDI cable. Needs the patch in [`emulator-patch/`](emulator-patch/).
* **`.gyb` / `.oka` through MIDI**, with an instrument list (F5) to reassign each slot while the song plays.
* **Tempo and key** (F7-F11) for every format; sound-module reset per song (F6); OPL virtual stereo (F12).
* **Visualisers**: 16-channel monitor with the instrument on each voice, and a piano roll.
* **Record to WAV**, and a **portable mode** that keeps settings beside the program (a `cfg` folder).
* **OPL register tunnel**: stream OPL writes to a Raspberry Pi over MIDI (see below).

## 🛠️ Building

Qt 6 (MinGW 64-bit) and CMake. The scripts pin the compiler, fetch AdPlug,
libbinio and munt on the first run, and assemble the release folder:

```cmd
git clone https://github.com/jihodaya/jmplayer.git
cd jmplayer
.\build_release.bat          :: Korean UI  -> release\
.\build_release_ENG.bat      :: English UI -> release_eng\
```

Details, and how to point the scripts at a Qt installed elsewhere, are in
[**BUILD_GUIDE.md**](BUILD_GUIDE.md).

## 📄 License & credits

* **Project code:** **MIT License** (see [`LICENSE`](LICENSE)).
* **Format notes** in `docs/formats/`: **CC BY 4.0** — reuse freely, but credit
  JJOME (jihodaya) and link back here.
* **Third-party code:** Qt 6 (LGPL-3.0) and munt (LGPL-2.1) are linked as DLLs.
  AdPlug, libbinio, Nuked-OPM and Nuked-OPN2 (LGPL-2.1) are compiled in - the
  complete source is here, so the program can be rebuilt against modified
  copies. emu2413 and emu2149 (MIT), ymfm and qsound-hle (BSD-3-Clause) are
  vendored with their licences. Everything is listed in [`LICENSE.md`](LICENSE.md).
* **Nuked-SC55** is a separate program, not part of JMPlayer - by
  [nukeykt](https://github.com/nukeykt/Nuked-SC55), with the
  [GUI fork](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float) by
  linoshkmalayil, under the MAME License (non-commercial). Only JMPlayer's own
  patch is included here.
* **Credits:** thanks to **BK (병코돌고래)** for the sample songs in `BK/`.

## 🔗 Companion projects

JMPlayer can stream its OPL2/OPL3 register writes to a Raspberry Pi over MIDI -
the **OPL register tunnel** - so a song played here comes out of the Pi's DAC.

| Project | What it is |
|---|---|
| **[mt32-extend](https://github.com/jihodaya/jukebox)** | Bare-metal Raspberry Pi jukebox, and a fork of [mt32-pi](https://github.com/dwhinham/mt32-pi) that receives the tunnel. It gives mt32-pi OPL playback it otherwise has no way to do. |
| **[rp2040-midi-bridge](https://github.com/jihodaya/rp2040-midi-bridge)** | USB MIDI ↔ serial MIDI bridge for the RP2040, one way to get the stream from the PC to the Pi. |

Neither is required to use JMPlayer on its own.

---

<a id="한국어"></a>

**JMPlayer**는 옛 컴퓨터 음악을 위한 Windows용 재생기입니다. 1990년대 국산
노래방·애드립 포맷, 일본 PC-98과 샤프 X68000의 시퀀서 파일, VGM 칩튠, 표준
MIDI를 재생합니다. 각 파일을 원래 의도된 음원으로 연주합니다. 에뮬레이션한
OPL3나 YM2151 같은 칩, 내장 MT-32, Nuked-SC55 에뮬레이터, 사운드폰트, 실제 MIDI
장치 중 알맞은 것을 쓰고, 가사가 들어 있는 포맷은 노래방 가사도 보여 줍니다.

## ⬇️ 다운로드

**[Releases](https://github.com/jihodaya/jmplayer/releases)** 에서 최신 빌드를
받으세요(Windows 10 / 11, 64비트). 압축을 풀고 실행하면 되며 설치 과정은
없습니다. 영문 화면은 `_eng` 파일을 받으세요.

MT-32 롬과 Nuked-SC55 에뮬레이터는 **포함되어 있지 않습니다**. 직접 준비한
파일을 어디에 넣는지는 매뉴얼을 참고하세요.

📖 **사용자 매뉴얼:** [한국어](docs/MANUAL.ko.md) · [English](docs/MANUAL.en.md)

🔍 **포맷 분석 기록:** [`docs/formats/`](docs/formats/README.md) — 각 파일 포맷(GYB,
OKA/OKM/OKW, NOB, ISS, RCP, Ballade SNG, MDX, MLD MDZ)에 대해 알아낸 것을 정리했습니다.
대부분 원본 프로그램을 디컴파일하고 원본이 만든 결과와 대조해서 알아낸 내용입니다.
CC BY 4.0으로 공개합니다.

## 🎵 지원 포맷

| 확장자 | 설명 | 연주 음원 |
|---|---|---|
| `.mid` `.midi` | 표준 MIDI | 사운드폰트, MIDI 장치, Nuked-SC55, 내장 MT-32 |
| `.nob` `.okm` `.okw` | 옥소리 노래방 (MIDI + 가사) | 위와 같음 |
| `.oka` `.gyb` | 옥소리 / 가요방 노래방 (OPL + 가사) | OPL3 에뮬레이션, 또는 MIDI |
| `.ims` (+`.iss`) `.rol` `.sop` | 애드립 음악 (IMS 가사는 `.iss`) | OPL3 에뮬레이션 (AdPlug) |
| `.rcp` | 레코포자 (PC-98 / X68000), `.GSD` 셋업 포함 | `.mid`와 같은 MIDI 음원 |
| `.sng` | 발라드 / ミュージくん / ミュージ郎 (PC-98) | `.mid`와 같은 MIDI 음원 |
| `.mdx` | 샤프 X68000 MXDRV, `.PDX` 샘플 뱅크 포함 | YM2151 + ADPCM |
| `.mdz` | 샤프 X68000 MLD | YM2151 + ADPCM, MIDI 동시 연주 |
| `.vgm` `.vgz` | VGM 칩 로그 | OPL2/OPL3, YM2612, YM2151, SN76489, 게임보이, YM2413, AY-3-8910, SegaPCM, YM2610, QSound |
| `.zip` | 위 파일들을 담은 압축 파일 | 자동으로 풀어서 재생 |

## 🌟 주요 기능

* **노래방 가사** — 조합형·완성형 가사를 음절 단위로 강조해 별도 창에 표시.
* **롤랜드 MT-32 / CM-32L 내장** (munt), 20자 디스플레이 창 포함. 롬은 직접 준비해야 합니다.
* **Nuked-SC55** 연결 — 가상 MIDI 케이블 없이 파이프로 연결. [`emulator-patch/`](emulator-patch/)의 패치로 빌드해야 합니다.
* **`.gyb` / `.oka`를 MIDI 음원으로** 연주하고, F5 악기 목록에서 재생 중에 악기를 바꿀 수 있습니다.
* **템포·키 조절**(F7–F11)이 모든 포맷에 적용, 곡마다 음원 리셋(F6), OPL 가상 스테레오(F12).
* **시각화** — 보이스마다 악기 이름이 보이는 16채널 모니터, 피아노 롤.
* **WAV 녹음**, 프로그램 옆 `cfg` 폴더에 설정을 두는 **휴대용 모드**.
* **OPL 레지스터 터널** — OPL 레지스터 쓰기를 MIDI로 라즈베리파이에 전송 (아래 참고).

## 🛠️ 빌드

Qt 6 (MinGW 64-bit)와 CMake가 필요합니다. 스크립트가 컴파일러 경로를 고정하고,
첫 실행 때 AdPlug·libbinio·munt를 받아 오며, 배포 폴더까지 만들어 줍니다.

```cmd
git clone https://github.com/jihodaya/jmplayer.git
cd jmplayer
.\build_release.bat          :: 한국어 UI -> release\
.\build_release_ENG.bat      :: 영어 UI   -> release_eng\
```

자세한 내용과 Qt를 다른 곳에 설치했을 때의 설정은
[**BUILD_GUIDE.md**](BUILD_GUIDE.md)를 참고하세요.

## 📄 라이선스 및 제공자 정보

* **프로젝트 소스 코드:** **MIT 라이선스**([`LICENSE`](LICENSE) 참고).
* **포맷 분석 기록**(`docs/formats/`): **CC BY 4.0** — 자유롭게 쓰되 작성자
  JJOME (jihodaya)와 출처를 밝혀 주세요.
* **외부 코드:** Qt 6(LGPL-3.0)와 munt(LGPL-2.1)는 DLL로 연결됩니다.
  AdPlug·libbinio·Nuked-OPM·Nuked-OPN2(LGPL-2.1)는 프로그램에 함께 컴파일되며,
  전체 소스가 이 저장소에 공개되어 있어 수정한 라이브러리로 다시 빌드할 수
  있습니다. emu2413·emu2149(MIT), ymfm·qsound-hle(BSD-3-Clause)는 라이선스
  파일과 함께 들어 있습니다. 전체 목록은 [`LICENSE.md`](LICENSE.md)에 있습니다.
* **Nuked-SC55:** JMPlayer의 일부가 아닌 별개의 프로그램입니다.
  [nukeykt](https://github.com/nukeykt/Nuked-SC55) 원작,
  [GUI 포크](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float)는
  linoshkmalayil, MAME 라이선스(비상업). 여기에는 JMPlayer의 패치만 들어 있습니다.
* **제공자 기여:** 샘플 연주곡(`BK/` 폴더)을 제공해 주신 **BK (병코돌고래)** 님께
  감사드립니다.

## 🔗 함께 쓰는 프로젝트

JMPlayer는 OPL2/OPL3 레지스터 쓰기를 MIDI로 라즈베리파이에 흘려보낼 수 있습니다
(**OPL 레지스터 터널**). 여기서 재생한 곡이 라즈베리파이의 DAC으로 나옵니다.

| 프로젝트 | 설명 |
|---|---|
| **[mt32-extend](https://github.com/jihodaya/jukebox)** | 라즈베리파이 베어메탈 쥬크박스와, 터널을 수신하는 [mt32-pi](https://github.com/dwhinham/mt32-pi) 포크. mt32-pi가 원래 못 하던 OPL 재생을 가능하게 합니다. |
| **[rp2040-midi-bridge](https://github.com/jihodaya/rp2040-midi-bridge)** | USB MIDI ↔ 시리얼 MIDI 브릿지(RP2040). PC에서 파이로 신호를 넘기는 방법 중 하나입니다. |

둘 다 JMPlayer 단독 사용에는 필요하지 않습니다.
