# JMPlayer R2.4c 빌드 및 배포 가이드

이 문서는 `build_release.bat` 배치 파일을 이용하여 Windows 환경에서 **JMPlayer R2.4c**의 릴리즈 배포 버전을 빌드하는 방법을 설명합니다.

## 1. 빌드 환경 요구사항 (Prerequisites)

이 프로젝트는 **Qt6 (LGPL v3)**와 C++를 기반으로 개발되었으며, Windows SDK MIDI API 및 OPL3 소프트웨어 신디사이저(AdPlug 등)를 연동합니다.

* **OS**: Windows 10 / 11 (64-bit)
* **Qt SDK**: **Qt 6.9.2 (MinGW 64-bit)**
  * 기본 설치 경로: `C:\Qt\6.9.2\mingw_64`
* **컴파일러 툴체인**: **MinGW-w64 13.1.0 (64-bit)**
  * Qt 설치 시 Tools에서 제공하는 버전 권장
  * 기본 설치 경로: `C:\Qt\Tools\mingw1310_64`
* **빌드 시스템**: CMake 3.26 이상 (Qt 설치 시 Ninja와 함께 자동 제공)

> **[참고]** 만약 Qt 설치 경로가 기본값과 다르다면, 프로젝트 루트의 [build_release.bat](file:///d:/py/midi-k-c260415/build_release.bat) 상단의 `QT_DIR` 및 `MINGW_DIR` 변수를 사용자의 실제 경로로 수정해 주어야 합니다.

---

## 2. 빌드 절차 (Build Steps)

1. **명령 프롬프트(cmd) 또는 PowerShell 실행**
2. **프로젝트 루트 디렉토리로 이동**
3. **릴리즈 빌드 스크립트 실행**:
   ```cmd
   .\build_release.bat
   ```

### 빌드 스크립트(`build_release.bat`)의 자동화 동작 순서:
1. **이전 빌드 정리**: 기존 `build/` 및 `release/` 폴더를 완전히 삭제하고 초기화합니다.
2. **CMake 환경 구성**: MinGW 64-bit 컴파일러를 명시적으로 타겟팅하여 CMake 설정을 구성합니다.
3. **소스 컴파일**: `cmake --build`를 통해 Release 최적화로 소스코드를 빌드합니다. (`build/MidiPlayer.exe` 생성)
4. **배포 패키지 구성 (`release/` 폴더)**:
   * 빌드된 바이너리를 `JMPlayer_R2.4c.exe` 명칭으로 복사
   * `K_icon.ico` (애플리케이션 아이콘) 복사
   * `IMS/STANDARD.BNK` 리소스 파일 복사 (OPL 재생을 위한 표준 뱅크)
   * `SoundFonts/` 폴더 전체 복사 (MIDI 신디사이저 재생용 기본 사운드폰트)
   * `BK/` 폴더 전체 복사 (기본 제공 샘플곡 세트)
5. **의존성 배포**: `windeployqt`를 구동하여 필요한 Qt6 DLL들을 `release/` 폴더로 자동 수집 및 배치합니다.

---

## 3. 배포 패키지 구조 (Release Structure)

빌드가 완료되면 생성되는 `release/` 폴더가 최종 배포 패키지입니다. 전체 폴더를 그대로 압축하여 배포합니다.

```
release/
├── JMPlayer_R2.4c.exe       # 플레이어 실행 파일
├── K_icon.ico               # 애플리케이션 아이콘
├── STANDARD.BNK             # OPL3 재생용 디폴트 뱅크 리소스 (필수)
├── BK/                      # 샘플 연주곡 디렉토리 (.ims, .iss, .rol)
├── SoundFonts/              # MIDI 렌더링용 사운드폰트 디렉토리 (.sf2)
│   ├── GeneralUser GS v1.511.sf2
│   └── VintageDreamsWaves-v2.sf2
├── platforms/               # Qt 윈도우 플랫폼 플러그인 (windeployqt 자동 복사)
│   └── qwindows.dll
├── Qt6Core.dll              # Qt 런타임 DLL (windeployqt 자동 복사)
├── Qt6Gui.dll
├── Qt6Widgets.dll
└── 기타 의존성 DLL 파일들...
```

---

## 4. 트러블슈팅 (Troubleshooting)

### CMake 구성 에러 또는 Compiler Mismatch
* **증상**: `version: 6.9.2 (64bit) rejection` 또는 `C Compiler not found` 에러가 발생하는 경우
* **원인**: 시스템 PATH 환경 변수에 과거 설치했던 다른 MinGW 툴체인(예: 32-bit gcc 등)이 혼재되어 우선순위로 매핑되었기 때문입니다.
* **해결**: [build_release.bat](file:///d:/py/midi-k-c260415/build_release.bat)는 내부적으로 컴파일러 경로를 `C:/Qt/Tools/mingw1310_64/bin/gcc.exe` 등으로 강제 고정하여 방지하므로, 반드시 `build_release.bat`을 통해 빌드하시기 바랍니다.

### DLL 누락 에러 ("Qt6Core.dll을 찾을 수 없습니다")
* **해결**: `release` 폴더 내부의 실행 파일이 아닌 개별 실행 파일만 외부로 빼서 실행한 경우입니다. 반드시 `release/` 폴더 전체가 한 자리에 있는 상태로 실행하셔야 합니다.
