# JMPlayer R2.4e — Build & Release Guide

This document explains how to build the release package of **JMPlayer R2.4e** on Windows using the provided batch scripts.

Two build scripts are provided:

| Script | UI Language | Build dir | Output |
|---|---|---|---|
| `build_release.bat` | Korean (default) | `build/` | `release/JMPlayer_R2.4e.exe` |
| `build_release_ENG.bat` | English (`-DENGLISH_UI=ON`) | `build_eng/` | `release_eng/JMPlayer_R2.4e_ENG.exe` |

> Only the player's interface text differs — song lyrics and playback are identical in both builds.

## 1. Prerequisites

The project is built with **Qt6 (LGPL v3)** and C++17, using the Windows MIDI API and software OPL3 synthesis (AdPlug).

* **OS**: Windows 10 / 11 (64-bit)
* **Qt SDK**: **Qt 6.9.2 (MinGW 64-bit)** — default path `C:\Qt\6.9.2\mingw_64`
* **Compiler**: **MinGW-w64 13.1.0 (64-bit)** — install via Qt Tools, default path `C:\Qt\Tools\mingw1310_64`
* **Build system**: CMake 3.16+ and GNU Make (both included with the Qt installer)
* **Internet connection (first build only)**: CMake **FetchContent** automatically downloads and patches the **AdPlug** and **libbinio** libraries from GitHub during the first configure. They are not bundled in this repository.

> **Note**: If your Qt is installed somewhere else, edit the `QT_DIR` and `MINGW_DIR` variables at the top of `build_release.bat` / `build_release_ENG.bat`.

## 2. Build Steps

1. Open Command Prompt (cmd) or PowerShell.
2. Run the script (it always operates from its own directory, so the current directory does not matter):
   ```cmd
   .\build_release.bat          (Korean UI)
   .\build_release_ENG.bat      (English UI)
   ```
   Add `nopause` to skip the final key-press prompt (useful for automation):
   ```cmd
   .\build_release.bat nopause
   ```

What the script does, in order:

1. **Clean** – deletes the previous `build/` and `release/` folders (`build_eng/` / `release_eng/` for the English script).
2. **Configure** – runs CMake with the MinGW 64-bit compilers explicitly pinned (the English script adds `-DENGLISH_UI=ON`). On the first run, AdPlug and libbinio are downloaded and patched here.
3. **Compile** – `cmake --build` in Release mode → `build/MidiPlayer.exe`.
4. **Package** – copies into the release folder: the renamed executable, `K_icon.ico`, `IMS/STANDARD.BNK` (default OPL3 bank), the `SoundFonts/` folder (MIDI rendering), and the `BK/` sample-song folder.
5. **Deploy Qt** – runs `windeployqt` to collect all required Qt6 DLLs and plugins into the release folder.

## 3. Release Package Structure

The finished `release/` (or `release_eng/`) folder is the final distributable — zip and ship the whole folder.

```
release/
├── JMPlayer_R2.4e.exe        # Player executable (JMPlayer_R2.4e_ENG.exe in release_eng/)
├── K_icon.ico                # Application icon
├── STANDARD.BNK              # Default OPL3 bank (required)
├── BK/                       # Bundled sample songs
├── SoundFonts/               # SoundFonts for MIDI rendering (.sf2)
│   ├── GeneralUser GS v1.511.sf2
│   └── VintageDreamsWaves-v2.sf2
├── platforms/                # Qt platform plugin (auto-copied by windeployqt)
│   └── qwindows.dll
├── Qt6Core.dll               # Qt runtime DLLs (auto-copied by windeployqt)
├── Qt6Gui.dll
├── Qt6Widgets.dll
└── ... other dependency DLLs
```

## 4. Troubleshooting

### CMake configure error / compiler mismatch
* **Symptom**: `version: 6.9.2 (64bit)` rejection or `C Compiler not found`.
* **Cause**: another MinGW toolchain (e.g. an old 32-bit gcc) earlier in your system PATH.
* **Fix**: always build through the provided scripts — they pin the compiler paths (`C:/Qt/Tools/mingw1310_64/bin/gcc.exe` etc.) explicitly.

### FetchContent download failure on first configure
* **Symptom**: CMake errors mentioning `libbinio` or `adplug` while configuring.
* **Cause**: no internet access, or GitHub unreachable.
* **Fix**: connect to the internet and re-run the script. After the first successful configure the sources are cached inside the build folder.

### Missing DLL error ("Qt6Core.dll was not found")
* **Cause**: the executable was moved out of the release folder by itself.
* **Fix**: keep the entire `release/` folder together; run the exe in place.

---

# JMPlayer R2.4e — 빌드 및 배포 가이드 (한국어)

이 문서는 제공되는 배치 스크립트로 Windows에서 **JMPlayer R2.4e** 릴리즈 배포판을 빌드하는 방법을 설명합니다.

빌드 스크립트는 두 가지입니다:

| 스크립트 | UI 언어 | 빌드 폴더 | 산출물 |
|---|---|---|---|
| `build_release.bat` | 한국어 (기본) | `build/` | `release/JMPlayer_R2.4e.exe` |
| `build_release_ENG.bat` | 영어 (`-DENGLISH_UI=ON`) | `build_eng/` | `release_eng/JMPlayer_R2.4e_ENG.exe` |

> 플레이어 화면의 표시 언어만 다르며, 곡 가사와 재생 기능은 두 빌드가 동일합니다.

## 1. 빌드 환경 요구사항

본 프로젝트는 **Qt6 (LGPL v3)** 와 C++17 기반이며, Windows MIDI API와 소프트웨어 OPL3 신디사이저(AdPlug)를 사용합니다.

* **OS**: Windows 10 / 11 (64-bit)
* **Qt SDK**: **Qt 6.9.2 (MinGW 64-bit)** — 기본 경로 `C:\Qt\6.9.2\mingw_64`
* **컴파일러**: **MinGW-w64 13.1.0 (64-bit)** — Qt 설치 시 Tools에서 제공, 기본 경로 `C:\Qt\Tools\mingw1310_64`
* **빌드 시스템**: CMake 3.16 이상 + GNU Make (Qt 설치 시 함께 제공)
* **인터넷 연결 (최초 빌드 시 필수)**: 최초 CMake 구성 단계에서 **FetchContent**가 **AdPlug**와 **libbinio** 라이브러리를 GitHub에서 자동 다운로드·패치합니다. 이 저장소에는 두 라이브러리가 포함되어 있지 않습니다.

> **참고**: Qt 설치 경로가 기본값과 다르면 `build_release.bat` / `build_release_ENG.bat` 상단의 `QT_DIR`, `MINGW_DIR` 변수를 실제 경로로 수정하세요.

## 2. 빌드 절차

1. 명령 프롬프트(cmd) 또는 PowerShell을 엽니다.
2. 스크립트를 실행합니다 (스크립트가 항상 자기 폴더 기준으로 동작하므로 현재 위치는 무관합니다):
   ```cmd
   .\build_release.bat          (한국어 UI)
   .\build_release_ENG.bat      (영어 UI)
   ```
   마지막 키 입력 대기를 생략하려면 `nopause` 인자를 붙입니다 (자동화에 유용):
   ```cmd
   .\build_release.bat nopause
   ```

스크립트 자동화 동작 순서:

1. **정리** – 기존 `build/`·`release/` 폴더 삭제 (영문 스크립트는 `build_eng/`·`release_eng/`).
2. **CMake 구성** – MinGW 64-bit 컴파일러 경로를 명시 고정하여 구성 (영문 스크립트는 `-DENGLISH_UI=ON` 추가). 최초 실행 시 이 단계에서 AdPlug/libbinio가 다운로드·패치됩니다.
3. **컴파일** – Release 모드로 `cmake --build` → `build/MidiPlayer.exe` 생성.
4. **패키지 구성** – 릴리즈 폴더에 복사: 이름 변경된 실행 파일, `K_icon.ico`, `IMS/STANDARD.BNK`(OPL3 기본 뱅크), `SoundFonts/`(MIDI 렌더링용), `BK/`(샘플곡).
5. **Qt 의존성 배포** – `windeployqt`가 필요한 Qt6 DLL·플러그인을 릴리즈 폴더에 자동 수집합니다.

## 3. 배포 패키지 구조

완성된 `release/` (또는 `release_eng/`) 폴더가 최종 배포 패키지입니다. 폴더 전체를 압축하여 배포하세요.

```
release/
├── JMPlayer_R2.4e.exe        # 플레이어 실행 파일 (release_eng/는 JMPlayer_R2.4e_ENG.exe)
├── K_icon.ico                # 애플리케이션 아이콘
├── STANDARD.BNK              # OPL3 기본 뱅크 (필수)
├── BK/                       # 기본 제공 샘플곡
├── SoundFonts/               # MIDI 렌더링용 사운드폰트 (.sf2)
│   ├── GeneralUser GS v1.511.sf2
│   └── VintageDreamsWaves-v2.sf2
├── platforms/                # Qt 플랫폼 플러그인 (windeployqt 자동 복사)
│   └── qwindows.dll
├── Qt6Core.dll               # Qt 런타임 DLL (windeployqt 자동 복사)
├── Qt6Gui.dll
├── Qt6Widgets.dll
└── 기타 의존성 DLL...
```

## 4. 트러블슈팅

### CMake 구성 에러 / 컴파일러 불일치
* **증상**: `version: 6.9.2 (64bit)` rejection 또는 `C Compiler not found`.
* **원인**: 시스템 PATH에 다른 MinGW 툴체인(예: 과거 설치한 32-bit gcc)이 먼저 잡히는 경우.
* **해결**: 반드시 제공된 스크립트로 빌드하세요 — 컴파일러 경로(`C:/Qt/Tools/mingw1310_64/bin/gcc.exe` 등)를 명시적으로 고정해 줍니다.

### 최초 구성 시 FetchContent 다운로드 실패
* **증상**: 구성 중 `libbinio` 또는 `adplug` 관련 CMake 에러.
* **원인**: 인터넷 미연결 또는 GitHub 접속 불가.
* **해결**: 인터넷 연결 후 스크립트를 다시 실행하세요. 최초 구성에 성공하면 소스가 빌드 폴더에 캐시됩니다.

### DLL 누락 에러 ("Qt6Core.dll을 찾을 수 없습니다")
* **원인**: 실행 파일만 릴리즈 폴더 밖으로 빼서 실행한 경우.
* **해결**: `release/` 폴더 전체를 유지한 상태에서 그 안의 실행 파일을 실행하세요.
