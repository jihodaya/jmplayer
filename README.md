# 🚀 JMPlayer (JJoMe MIDI-OPL Player)
<img width="1372" height="675" alt="image" src="https://github.com/user-attachments/assets/24fafd45-3aa8-4a48-8774-9296c70173a9" />
<img width="520" height="293" alt="JMPLAYER_R24b" src="https://github.com/user-attachments/assets/bf4209e2-bff3-4e56-b502-bb252f41726e" />


https://www.youtube.com/@jjome_Plus

**JMPlayer** is a simple MIDI and retro chiptune player written in C++ using the Qt6 framework. It is built to play and visualize 1990s Korean retro music formats (such as `.SOP`, `.GYB`, `.NOB`, `.OKM` / `.OKA`, and `.IMS` / `.ISS`) along with standard `.ROL` (AdLib) and VGM/VGZ chiptunes, utilizing software OPL3 emulation and SoundFont engines.

### 🌟 Features

*   **Format Support:** Playback and parsing of Korean retro formats (`.SOP`, `.GYB`, `.NOB`, `.OKM` / `.OKA`, `.IMS` / `.ISS`), standard AdLib `.ROL` files, and VGM/VGZ chiptunes.
*   **Visualizer:** 
    *   16-channel volume VU meters and attack peak visualization.
    *   Piano Roll window showing active note layouts.
    *   Dynamic instrument name cards displaying active FM patches.
*   **Lyrics Display:** Basic lyrics support with syllable-level highlighting and text decoding (Johab/EUC-KR).
*   **Audio Engine:** Software OPL3 synthesis via AdPlug and SoundFont (.sf2) playback powered by TinySoundFont.

### 🛠️ Quick Build Guide

This project requires CMake and Qt6 (MinGW 64-bit).

```cmd
# 1. Clone the repository
git clone https://github.com/yourusername/JMPlayer.git
cd JMPlayer

# 2. Run the automated release build script (Windows)
.\build_release.bat
```
*For detailed instructions, please refer to [**BUILD_GUIDE.md**](BUILD_GUIDE.md).*

### 📄 License & Credits

*   **Project Code:** Released under the Public Domain. Anyone is free to use, modify, and distribute.
*   **Third-party Dependencies:** Dynamically linked to **Qt 6 (LGPL v3)** and **AdPlug (LGPL v2.1)** to comply with LGPL requirements.
*   **Credits:** Thanks to **BK (병코돌고래)** for providing the sample song collection (`BK/` folder).

---



**JMPlayer**는 Qt6 프레임워크와 C++를 기반으로 개발된 미디(MIDI) 및 레트로 칩튠 재생기입니다. 1990년대 PC 통신 시절에 주로 쓰이던 국산 고전 음악 파일 포맷들(`.SOP`, `.GYB`, `.NOB`, `.OKM/OKA`, `.IMS/ISS`)과 표준 애드립 포맷인 `.ROL`, 그리고 VGM/VGZ 칩튠 파일들을 OPL3 에뮬레이션과 사운드폰트(SoundFont) 엔진을 활용하여 재생하고 시각화합니다.

### 🌟 주요 기능

*   **포맷 재생 지원:** 국산 고전 포맷 파일(`.SOP`, `.GYB`, `.NOB`, `.OKM` / `.OKA`, `.IMS` / `.ISS`) 및 표준 애드립 파일(`.ROL`), VGM/VGZ 칩튠 파일 재생.
*   **시각화 (Visualizer):**
    *   16채널 볼륨 VU 레벨 모니터 및 어택 Peak 표시.
    *   음표 연주 상태를 보여주는 피아노 롤(Piano Roll) 창.
    *   활성화된 FM 패치 정보를 보여주는 악기 이름 카드.
*   **가사 표시:** 조합형(Johab) 및 완결형(EUC-KR) 가사 텍스트의 음절 단위 하이라이팅 표시.
*   **오디오 엔진:** AdPlug 기반의 소프트웨어 OPL3 에뮬레이션 및 TinySoundFont 기반의 사운드폰트(.sf2) 재생.

### 🛠️ 빠른 빌드 방법

본 프로젝트는 CMake 및 Qt6 (MinGW 64-bit) 환경에서 빌드됩니다.

```cmd
# 1. 레포지토리 클론
git clone https://github.com/yourusername/JMPlayer.git
cd JMPlayer

# 2. 윈도우용 릴리즈 빌드 자동화 스크립트 실행
.\build_release.bat
```
*보다 상세한 빌드 방법은 [**BUILD_GUIDE.md**](BUILD_GUIDE.md) 문서를 참고해 주세요.*

### 📄 라이선스 및 제공자 정보

*   **프로젝트 소스 코드:** 퍼블릭 도메인(Public Domain)으로 배포됩니다. 자유롭게 사용, 수정 및 배포하실 수 있습니다.
*   **외부 라이브러리:** LGPL 라이선스 준수를 위해 **Qt 6 (LGPL v3)** 및 **AdPlug (LGPL v2.1)** 라이브러리와 동적 링크(DLL) 방식으로 빌드됩니다.
*   **제공자 기여:** 음원 테스트를 위해 샘플 연주곡 세트(`BK/` 폴더)를 제공해 주신 **BK (병코돌고래)** 님께 감사드립니다.
