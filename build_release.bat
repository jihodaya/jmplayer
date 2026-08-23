@echo off
if "%1"=="nopause" set NO_PAUSE=1
if "%1"=="--no-pause" set NO_PAUSE=1
REM Always operate from this script's own directory (CWD-safe)
cd /d "%~dp0"
REM ========================================
REM JJoMe MIDI Player - License Compliant Build
REM Qt6 (LGPL) + NOB Player + Sound-Module Reset + Nuked-SC55 support
REM ========================================
REM
REM   Source (CMakeLists + assets) : .\  (this folder)
REM   Build dir                    : .\build\
REM   Release output               : .\release\
REM ========================================

REM Absolute paths so cmake/copy never depend on the current directory.
set SCRIPT_DIR=%~dp0
set SRC_DIR=%SCRIPT_DIR%.
set BUILD_DIR=%SCRIPT_DIR%build
set RELEASE_DIR=%SCRIPT_DIR%release

echo.
echo ========================================
echo JJoMe MIDI Player V3.0.0 beta
echo License-Compliant Build Script
echo ========================================
echo.
echo Source : %SRC_DIR%
echo Build  : %BUILD_DIR%
echo Output : %RELEASE_DIR%
echo.

REM Toolchain locations. These are DEFAULTS, not fixed paths: set QT_DIR and
REM MINGW_DIR in the environment beforehand and yours are used instead, so the
REM script works on a machine that keeps Qt somewhere else.
REM
REM   set QT_DIR=D:\Qt\6.9.2\mingw_64
REM   set MINGW_DIR=D:\Qt\Tools\mingw1310_64
REM   build_release.bat
if not defined QT_DIR    set QT_DIR=C:\Qt\6.9.2\mingw_64
if not defined MINGW_DIR set MINGW_DIR=C:\Qt\Tools\mingw1310_64
set PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%

echo Setting up Qt6 environment...
echo Qt Directory: %QT_DIR%
echo MinGW Directory: %MINGW_DIR%
echo.

if not exist "%QT_DIR%\bin\qmake.exe" (
    echo [ERROR] Qt6 not found at: %QT_DIR%
    echo         Set QT_DIR to your Qt mingw_64 folder and run again.
    if not defined NO_PAUSE pause
    exit /b 1
)
if not exist "%MINGW_DIR%\bin\g++.exe" (
    echo [ERROR] MinGW not found at: %MINGW_DIR%
    echo         Set MINGW_DIR to your Qt MinGW toolchain folder and run again.
    if not defined NO_PAUSE pause
    exit /b 1
)

REM Clean previous build (only THIS folder's build/release)
echo [1/5] Cleaning previous build...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%BUILD_DIR%"
echo Done.
echo.

REM Configure (out-of-source: build dir here, source dir is the parent jmp\)
echo [2/5] Configuring with CMake...
cd /d "%BUILD_DIR%"
REM Explicit compiler paths prevent CMake from auto-detecting 32-bit gcc
REM (e.g. C:\MinGW\bin\gcc.exe), which causes a Qt6 64-bit mismatch failure
REM with the message: "version: 6.9.2 (64bit)" rejection.
REM Forward slashes: CMake treats a backslash as an escape, and a Windows path
REM pasted straight in makes CMakeRCCompiler.cmake fail to parse on the next run.
set "QT_DIR_FS=%QT_DIR:\=/%"
set "MINGW_DIR_FS=%MINGW_DIR:\=/%"

REM munt (libmt32emu), for the MT-32 engine. A local checkout is used when it is
REM there so an ordinary build needs no network; CMake clones it otherwise.
if not defined MUNT_DIR set "MUNT_DIR=%SCRIPT_DIR%..\src\munt"
if exist "%MUNT_DIR%\mt32emu\CMakeLists.txt" (
    set "MUNT_DIR_FS=%MUNT_DIR:\=/%"
) else (
    set "MUNT_DIR_FS="
)

cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER="%MINGW_DIR_FS%/bin/gcc.exe" ^
      -DCMAKE_CXX_COMPILER="%MINGW_DIR_FS%/bin/g++.exe" ^
      -DCMAKE_RC_COMPILER="%MINGW_DIR_FS%/bin/windres.exe" ^
      -DCMAKE_PREFIX_PATH="%QT_DIR_FS%" ^
      -DFETCHCONTENT_SOURCE_DIR_MUNT="%MUNT_DIR_FS%" "%SRC_DIR%"

if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed!
    cd /d "%SCRIPT_DIR%"
    if not defined NO_PAUSE pause
    exit /b 1
)
echo Done.
echo.

REM Build
echo [3/5] Building project...
cmake --build . --config Release -j 4

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    cd /d "%SCRIPT_DIR%"
    if not defined NO_PAUSE pause
    exit /b 1
)
cd /d "%SCRIPT_DIR%"
echo Done.
echo.

REM Prepare release folder
mkdir "%RELEASE_DIR%"

REM Three banks travel with the player, all managed from IMS\ .
REM   GAYO.BNK     - GAYOBANG's own, 2,154. Read first for .GYB.
REM   NORE.BNK     - NORE45's own, 6,009. Read first for .OKA.
REM   STANDARD.BNK - the general OPL collection, 15,867, for .IMS/.ROL.
REM                  Not part of the .GYB/.OKA chain: neither DOS
REM                  program ever read it.
REM The two DOS banks share all but one name, but 244 of the shared names hold
REM different parameters, so which one a song is read against matters; see
REM bnkfill.cpp. Files converted from .ROL carry instrument names with no
REM parameters and are silent without a bank at all.
if exist "%SRC_DIR%\IMS\GAYO.BNK" (
    copy "%SRC_DIR%\IMS\GAYO.BNK" "%RELEASE_DIR%\" > nul
)
if exist "%SRC_DIR%\IMS\NORE.BNK" (
    copy "%SRC_DIR%\IMS\NORE.BNK" "%RELEASE_DIR%\" > nul
)

if exist "%SRC_DIR%\IMS\STANDARD.BNK" (
    echo [4/5] Copying standard bank file...
    copy "%SRC_DIR%\IMS\STANDARD.BNK" "%RELEASE_DIR%\" > nul
) else (
    echo [4/5] Warning: IMS\STANDARD.BNK not found.
)

echo [5/5] Creating directory structure for release...
copy "%BUILD_DIR%\MidiPlayer.exe" "%RELEASE_DIR%\JMPlayer_V3.0.0-beta.exe"
copy "%SRC_DIR%\K_icon.ico" "%RELEASE_DIR%\K_icon.ico"
if exist "%SRC_DIR%\SoundFonts" xcopy "%SRC_DIR%\SoundFonts" "%RELEASE_DIR%\SoundFonts" /E /I /Y
if exist "%SRC_DIR%\BK" xcopy "%SRC_DIR%\BK" "%RELEASE_DIR%\BK" /E /I /Y
if exist "%SRC_DIR%\etc\LICENSES.md" copy "%SRC_DIR%\etc\LICENSES.md" "%RELEASE_DIR%\LICENSES.md"
REM PDF manuals. The previous line here named a file called ".pdf" - the Korean
REM part of the filename had been lost somewhere - so the manual never actually
REM reached a release. ASCII names now, for the same reason kernel images use
REM them: a name that survives every tool in the chain.
if exist "%SRC_DIR%\etc\JMPlayer_Manual_KO.pdf" copy "%SRC_DIR%\etc\JMPlayer_Manual_KO.pdf" "%RELEASE_DIR%\" > nul
if exist "%SRC_DIR%\etc\JMPlayer_Manual_EN.pdf" copy "%SRC_DIR%\etc\JMPlayer_Manual_EN.pdf" "%RELEASE_DIR%\" > nul

REM libmt32emu, the MT-32 engine. Unlike Nuked-SC55 this one DOES ship: it is
REM LGPL-2.1, so a public-domain program may link it dynamically and pass it on.
REM Only the ROMs are missing, and those are Roland's.
for %%D in (libmt32emu-2.dll libmt32emu.dll) do (
    if exist "%BUILD_DIR%\_deps\munt-build\%%D" copy "%BUILD_DIR%\_deps\munt-build\%%D" "%RELEASE_DIR%\\" > nul
)
if not exist "%RELEASE_DIR%\MT32ROMs" mkdir "%RELEASE_DIR%\MT32ROMs"
REM Both this name and the file's contents are ASCII on purpose. cmd.exe reads
REM a batch file's bytes in the console OEM codepage (CP949 on a Korean
REM install) while this file is UTF-8, so a Korean literal here reaches the
REM filesystem as mojibake - this line used to name the copy in Korean and
REM produced an unopenable name in MT32ROMs (reported 2026-08-21).
REM NukedSC55\README.txt next to it has always been ASCII and has always been
REM fine. Keep every literal in these scripts ASCII.
copy "%SCRIPT_DIR%\MT32ROMs_README.txt" "%RELEASE_DIR%\MT32ROMs\README.txt" > nul

REM Nuked-SC55 drop folder. The emulator itself is NOT shipped - it is not
REM public domain and jmp is - so only the folder and its note go out.
if not exist "%RELEASE_DIR%\NukedSC55" mkdir "%RELEASE_DIR%\NukedSC55"
copy "%SCRIPT_DIR%\NukedSC55_README.txt" "%RELEASE_DIR%\NukedSC55\README.txt"
if exist "%SCRIPT_DIR%\emulator-patch" xcopy "%SCRIPT_DIR%\emulator-patch" "%RELEASE_DIR%\emulator-patch" /E /I /Y

if errorlevel 1 (
    echo.
    echo [ERROR] Failed to copy executable!
    if not defined NO_PAUSE pause
    exit /b 1
)
echo Done.
echo.

REM Deploy Qt dependencies
echo [5/5] Deploying Qt6 dependencies...
cd /d "%RELEASE_DIR%"
windeployqt JMPlayer_V3.0.0-beta.exe --release --no-translations --no-opengl-sw

if errorlevel 1 (
    echo.
    echo [WARNING] windeployqt had issues, but continuing...
)

cd /d "%SCRIPT_DIR%"
echo Done.
echo.

REM Show results
echo ========================================
echo BUILD COMPLETED SUCCESSFULLY!
echo ========================================
echo.
echo Release folder: %RELEASE_DIR%\
echo Executable: %RELEASE_DIR%\JMPlayer_V3.0.0-beta.exe
echo.

if exist "%RELEASE_DIR%\JMPlayer_V3.0.0-beta.exe" (
    for %%A in ("%RELEASE_DIR%\JMPlayer_V3.0.0-beta.exe") do (
        echo Executable size: %%~zA bytes
    )
)

echo.
if not defined NO_PAUSE pause
