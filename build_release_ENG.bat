@echo off
if "%1"=="nopause" set NO_PAUSE=1
if "%1"=="--no-pause" set NO_PAUSE=1
REM Always operate from this script's own directory (CWD-safe)
cd /d "%~dp0"
REM ========================================
REM JJoMe MIDI Player - License Compliant Build (ENGLISH UI)
REM Qt6 (LGPL) + NOB Player
REM Builds with -DENGLISH_UI=ON into build_eng\ and release_eng\
REM (Song lyrics are unaffected; only the player UI text is English.)
REM ========================================

echo.
echo ========================================
echo JJoMe MIDI Player R2.4d  (English UI)
echo License-Compliant Build Script
echo ========================================
echo.
echo This build uses:
echo - Qt 6 (LGPL v3) - Dynamically linked
echo - Windows MIDI API - System library
echo - K_icon.ico - Application icon
echo - ENGLISH_UI = ON
echo.

REM Set environment paths
set QT_DIR=C:\Qt\6.9.2\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1310_64
set PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%

echo Setting up Qt6 environment...
echo Qt Directory: %QT_DIR%
echo MinGW Directory: %MINGW_DIR%
echo.

REM Clean previous build
echo [1/5] Cleaning previous build...
if exist build_eng rmdir /s /q build_eng
if exist release_eng rmdir /s /q release_eng
mkdir build_eng
echo Done.
echo.

REM Configure
echo [2/5] Configuring with CMake (ENGLISH_UI=ON)...
cd /D %~dp0build_eng
REM Explicit compiler paths prevent CMake from auto-detecting 32-bit gcc
REM (e.g. C:\MinGW\bin\gcc.exe), which causes a Qt6 64-bit mismatch failure
REM with the message: "version: 6.9.2 (64bit)" rejection.
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DENGLISH_UI=ON ^
      -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
      -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe ^
      -DCMAKE_RC_COMPILER=C:/Qt/Tools/mingw1310_64/bin/windres.exe ^
      -DCMAKE_PREFIX_PATH=%QT_DIR% ..

if errorlevel 1 (
    echo.
    echo [ERROR] CMake configuration failed!
    cd ..
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
    cd ..
    if not defined NO_PAUSE pause
    exit /b 1
)
cd /D %~dp0
echo Done.
echo.

REM Prepare release folder
mkdir release_eng

if exist IMS\STANDARD.BNK (
    echo [4/5] Copying standard bank file...
    copy IMS\STANDARD.BNK release_eng\ > nul
) else (
    echo [4/5] Warning: IMS\STANDARD.BNK not found.
)


echo [5/5] Creating directory structure for release...
copy build_eng\MidiPlayer.exe release_eng\JMPlayer_R2.4d_ENG.exe
copy K_icon.ico release_eng\K_icon.ico
if exist SoundFonts xcopy SoundFonts release_eng\SoundFonts /E /I /Y
if exist BK xcopy BK release_eng\BK /E /I /Y
if exist etc\LICENSES.md copy etc\LICENSES.md release_eng\LICENSES.md
if exist etc\.pdf copy etc\.pdf release_eng\.pdf

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
cd /D %~dp0release_eng
windeployqt JMPlayer_R2.4d_ENG.exe --release --no-translations --no-opengl-sw

if errorlevel 1 (
    echo.
    echo [WARNING] windeployqt had issues, but continuing...
)

cd /D %~dp0
echo Done.
echo.

REM Show results
echo ========================================
echo BUILD COMPLETED SUCCESSFULLY!  (English UI)
echo ========================================
echo.
echo Release folder: .\release_eng\
echo Executable: .\release_eng\JMPlayer_R2.4d_ENG.exe
echo Icon: .\release_eng\K_icon.ico
echo.

if exist release_eng\JMPlayer_R2.4d_ENG.exe (
    for %%A in (release_eng\JMPlayer_R2.4d_ENG.exe) do (
        echo Executable size: %%~zA bytes
    )
)

echo.
echo Distribution Instructions:
echo 1. Distribute the ENTIRE 'release_eng' folder
echo 2. Include Qt6 DLLs (automatically copied by windeployqt)
echo 3. Include K_icon.ico
echo 4. Include LICENSE file (Qt LGPL + project license)
echo 5. Provide source code access (GitHub link or zip)

echo.
echo LGPL Compliance:
echo - Qt6 libraries are dynamically linked (DLL files)
echo - Source code is available at [Your GitHub URL]
echo - Users can replace Qt libraries with their own builds
echo.

if not defined NO_PAUSE pause
