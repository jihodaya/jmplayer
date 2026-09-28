# JMPlayer — Build & Release Guide

This document explains how to build the release package of **JMPlayer** on
Windows using the provided batch scripts. The version number is in the
executable's name (`JMPlayer_V3.1.1.exe` at the time of writing).

Two build scripts are provided:

| Script | UI Language | Build dir | Output |
|---|---|---|---|
| `build_release.bat` | Korean (default) | `build/` | `release/JMPlayer_<version>.exe` |
| `build_release_ENG.bat` | English (`-DENGLISH_UI=ON`) | `build_eng/` | `release_eng/JMPlayer_<version>_ENG.exe` |

> Only the player's interface text differs — song lyrics and playback are identical in both builds.

## 1. Prerequisites

* **OS**: Windows 10 / 11 (64-bit)
* **Qt SDK**: **Qt 6.9.2 (MinGW 64-bit)** — default path `C:\Qt\6.9.2\mingw_64`
* **Compiler**: **MinGW-w64 13.1.0 (64-bit)** — install via Qt Tools, default path `C:\Qt\Tools\mingw1310_64`
* **Build system**: CMake 3.16+ and GNU Make (both included with the Qt installer)
* **Internet connection (first build only)**: CMake **FetchContent** downloads
  **AdPlug**, **libbinio** and **munt** (the MT-32 engine) from GitHub during
  the first configure. They are not bundled in this repository. If a munt
  checkout already sits at `..\src\munt`, it is used instead of downloading.

Everything else — the YM2151, YM2612, YM2413, AY-3-8910, YM2610 and QSound
cores — is already in the repository (`mdxcore/`, `vgmcore/`).

### Qt installed somewhere else

The paths above are defaults. Set `QT_DIR` and `MINGW_DIR` before running a
script and yours are used instead — no need to edit the script:

```cmd
set QT_DIR=D:\Qt\6.9.2\mingw_64
set MINGW_DIR=D:\Qt\Tools\mingw1310_64
build_release.bat
```

## 2. Build Steps

1. **Close JMPlayer if it is running.** The script deletes the old release
   folder first, and Windows will not delete a running `.exe` — the build then
   reports success while the old executable stays in place.
2. Open Command Prompt (cmd) or PowerShell and run a script (it always
   operates from its own directory, so the current directory does not matter):
   ```cmd
   .\build_release.bat          (Korean UI)
   .\build_release_ENG.bat      (English UI)
   ```
   Add `nopause` to skip the final key-press prompt:
   ```cmd
   .\build_release.bat nopause
   ```

What the script does, in order:

1. **Clean** – deletes the previous `build/` and `release/` folders (`build_eng/` / `release_eng/` for the English script).
2. **Configure** – runs CMake with the MinGW 64-bit compilers explicitly pinned (the English script adds `-DENGLISH_UI=ON`). On the first run AdPlug, libbinio and munt are downloaded here.
3. **Compile** – `cmake --build` in Release mode → `build/MidiPlayer.exe`.
4. **Package** – copies the renamed executable and everything it needs into the release folder (see below).
5. **Deploy Qt** – runs `windeployqt` to collect the Qt6 DLLs and plugins.

## 3. Release Package Structure

The finished `release/` (or `release_eng/`) folder is the final distributable —
zip and ship the whole folder.

```
release/
├── JMPlayer_<version>.exe    # Player executable (..._ENG.exe in release_eng/)
├── K_icon.ico                # Application icon
├── STANDARD.BNK              # OPL instrument bank for .ims / .rol
├── GAYO.BNK, NORE.BNK        # Gayobang / NORE45 banks for .gyb / .oka
├── libmt32emu-2.dll          # MT-32 engine (munt, LGPL-2.1)
├── LICENSE.txt               # JMPlayer's MIT licence
├── LICENSES.md               # Third-party licences
├── JMPlayer_Manual_KO.pdf    # User manuals
├── JMPlayer_Manual_EN.pdf
├── BK/                       # Bundled sample songs
├── SoundFonts/               # SoundFonts for MIDI rendering (.sf2)
├── MT32ROMs/                 # Put your own MT-32 / CM-32L ROMs here (README.txt only)
├── NukedSC55/                # Put your own Nuked-SC55 build here (README.txt only)
├── emulator-patch/           # The patch Nuked-SC55 needs to work with JMPlayer
├── platforms/, styles/, ...  # Qt plugins (auto-copied by windeployqt)
├── Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, ...
└── ... other dependency DLLs
```

MT-32 ROMs and the Nuked-SC55 emulator are **never** part of the package: the
ROMs are Roland's, and the emulator's licence does not allow redistribution.

## 4. Troubleshooting

### CMake configure error / compiler mismatch
* **Symptom**: `version: 6.9.2 (64bit)` rejection or `C Compiler not found`.
* **Cause**: another MinGW toolchain (e.g. an old 32-bit gcc) earlier in your system PATH.
* **Fix**: always build through the provided scripts — they pin the compiler paths explicitly.

### FetchContent download failure on first configure
* **Symptom**: CMake errors mentioning `libbinio`, `adplug` or `munt` while configuring.
* **Cause**: no internet access, or GitHub unreachable.
* **Fix**: connect to the internet and re-run the script. After the first successful configure the sources are cached inside the build folder.

### The build "succeeded" but nothing changed
* **Cause**: JMPlayer was still running, so the old executable could not be replaced.
* **Fix**: close it and build again; check the executable's modified time.

### Missing DLL error ("Qt6Core.dll was not found")
* **Cause**: the executable was moved out of the release folder by itself.
* **Fix**: keep the entire `release/` folder together; run the exe in place.

---

# JMPlayer — 빌드 및 배포 가이드 (한국어)

이 문서는 제공되는 배치 스크립트로 Windows에서 **JMPlayer** 배포판을 빌드하는
방법을 설명합니다. 버전은 실행 파일 이름에 들어갑니다(작성 시점 기준
`JMPlayer_V3.1.1.exe`).

빌드 스크립트는 두 가지입니다:

| 스크립트 | UI 언어 | 빌드 폴더 | 산출물 |
|---|---|---|---|
| `build_release.bat` | 한국어 (기본) | `build/` | `release/JMPlayer_<버전>.exe` |
| `build_release_ENG.bat` | 영어 (`-DENGLISH_UI=ON`) | `build_eng/` | `release_eng/JMPlayer_<버전>_ENG.exe` |

> 플레이어 화면의 표시 언어만 다르며, 곡 가사와 재생 기능은 두 빌드가 동일합니다.

## 1. 빌드 환경 요구사항

* **OS**: Windows 10 / 11 (64-bit)
* **Qt SDK**: **Qt 6.9.2 (MinGW 64-bit)** — 기본 경로 `C:\Qt\6.9.2\mingw_64`
* **컴파일러**: **MinGW-w64 13.1.0 (64-bit)** — Qt 설치 시 Tools에서 제공, 기본 경로 `C:\Qt\Tools\mingw1310_64`
* **빌드 시스템**: CMake 3.16 이상 + GNU Make (Qt 설치 시 함께 제공)
* **인터넷 연결 (최초 빌드 시 필수)**: 최초 CMake 구성 단계에서
  **FetchContent**가 **AdPlug**, **libbinio**, **munt**(MT-32 엔진)를
  GitHub에서 받아 옵니다. 이 저장소에는 포함되어 있지 않습니다. `..\src\munt`에
  munt가 이미 있으면 받지 않고 그것을 씁니다.

그 밖의 YM2151, YM2612, YM2413, AY-3-8910, YM2610, QSound 코어는 이미 저장소에
들어 있습니다(`mdxcore/`, `vgmcore/`).

### Qt를 다른 곳에 설치한 경우

위 경로는 기본값입니다. 스크립트를 실행하기 전에 `QT_DIR`, `MINGW_DIR`을
지정하면 그 경로를 씁니다. 스크립트를 고칠 필요는 없습니다.

```cmd
set QT_DIR=D:\Qt\6.9.2\mingw_64
set MINGW_DIR=D:\Qt\Tools\mingw1310_64
build_release.bat
```

## 2. 빌드 절차

1. **JMPlayer가 실행 중이면 먼저 종료하세요.** 스크립트는 기존 배포 폴더부터
   지우는데, Windows는 실행 중인 `.exe`를 지우지 못합니다. 그러면 빌드는 성공으로
   나오지만 실행 파일은 예전 것 그대로 남습니다.
2. 명령 프롬프트(cmd) 또는 PowerShell에서 스크립트를 실행합니다(스크립트가 항상
   자기 폴더 기준으로 동작하므로 현재 위치는 무관합니다):
   ```cmd
   .\build_release.bat          (한국어 UI)
   .\build_release_ENG.bat      (영어 UI)
   ```
   마지막 키 입력 대기를 생략하려면 `nopause` 인자를 붙입니다:
   ```cmd
   .\build_release.bat nopause
   ```

스크립트 동작 순서:

1. **정리** – 기존 `build/`·`release/` 폴더 삭제 (영문 스크립트는 `build_eng/`·`release_eng/`).
2. **CMake 구성** – MinGW 64-bit 컴파일러 경로를 고정하여 구성 (영문 스크립트는 `-DENGLISH_UI=ON` 추가). 최초 실행 시 이 단계에서 AdPlug·libbinio·munt를 받아 옵니다.
3. **컴파일** – Release 모드로 `cmake --build` → `build/MidiPlayer.exe` 생성.
4. **패키지 구성** – 이름을 바꾼 실행 파일과 필요한 파일들을 배포 폴더에 복사합니다(아래 참고).
5. **Qt 의존성 배포** – `windeployqt`가 필요한 Qt6 DLL·플러그인을 모읍니다.

## 3. 배포 패키지 구조

완성된 `release/` (또는 `release_eng/`) 폴더가 최종 배포 패키지입니다. 폴더
전체를 압축하여 배포하세요.

```
release/
├── JMPlayer_<버전>.exe       # 플레이어 실행 파일 (release_eng/는 ..._ENG.exe)
├── K_icon.ico                # 애플리케이션 아이콘
├── STANDARD.BNK              # .ims / .rol용 OPL 악기 뱅크
├── GAYO.BNK, NORE.BNK        # .gyb / .oka용 가요방·NORE45 뱅크
├── libmt32emu-2.dll          # MT-32 엔진 (munt, LGPL-2.1)
├── LICENSE.txt               # JMPlayer의 MIT 라이선스
├── LICENSES.md               # 외부 라이브러리 라이선스
├── JMPlayer_Manual_KO.pdf    # 사용자 매뉴얼
├── JMPlayer_Manual_EN.pdf
├── BK/                       # 기본 제공 샘플곡
├── SoundFonts/               # MIDI 렌더링용 사운드폰트 (.sf2)
├── MT32ROMs/                 # 직접 준비한 MT-32 / CM-32L 롬을 넣는 곳 (README.txt만 있음)
├── NukedSC55/                # 직접 빌드한 Nuked-SC55를 넣는 곳 (README.txt만 있음)
├── emulator-patch/           # Nuked-SC55를 JMPlayer와 쓰기 위한 패치
├── platforms/, styles/, ...  # Qt 플러그인 (windeployqt 자동 복사)
├── Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll, ...
└── 기타 의존성 DLL...
```

MT-32 롬과 Nuked-SC55 에뮬레이터는 **절대** 배포판에 들어가지 않습니다. 롬은
롤랜드의 저작물이고, 에뮬레이터는 라이선스상 재배포할 수 없습니다.

## 4. 트러블슈팅

### CMake 구성 에러 / 컴파일러 불일치
* **증상**: `version: 6.9.2 (64bit)` rejection 또는 `C Compiler not found`.
* **원인**: 시스템 PATH에 다른 MinGW 툴체인(예: 과거 설치한 32-bit gcc)이 먼저 잡히는 경우.
* **해결**: 반드시 제공된 스크립트로 빌드하세요 — 컴파일러 경로를 명시적으로 고정해 줍니다.

### 최초 구성 시 FetchContent 다운로드 실패
* **증상**: 구성 중 `libbinio`, `adplug`, `munt` 관련 CMake 에러.
* **원인**: 인터넷 미연결 또는 GitHub 접속 불가.
* **해결**: 인터넷 연결 후 스크립트를 다시 실행하세요. 최초 구성에 성공하면 소스가 빌드 폴더에 캐시됩니다.

### 빌드는 성공했는데 바뀐 게 없음
* **원인**: JMPlayer가 실행 중이어서 예전 실행 파일이 교체되지 않았습니다.
* **해결**: 프로그램을 종료하고 다시 빌드한 뒤, 실행 파일의 수정 시각을 확인하세요.

### DLL 누락 에러 ("Qt6Core.dll을 찾을 수 없습니다")
* **원인**: 실행 파일만 배포 폴더 밖으로 빼서 실행한 경우.
* **해결**: `release/` 폴더 전체를 유지한 상태에서 그 안의 실행 파일을 실행하세요.
