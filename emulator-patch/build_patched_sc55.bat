@echo off
setlocal enabledelayedexpansion
if "%1"=="nopause" set NO_PAUSE=1
cd /d "%~dp0"
REM ========================================
REM Build the PATCHED Nuked-SC55 for jmp
REM ========================================
REM
REM jmp drives Nuked-SC55 over a named pipe (no loopMIDI). A STOCK build stays
REM silent - the patch here routes the pipe bytes to the main MCU's MIDI input.
REM This script applies the patch and builds it, then drops the result into
REM jmp's NukedSC55 folder. You still add ROM files yourself (Roland's).
REM
REM What it does:
REM   1. Clone Nuked-SC55-GUI-Float (if not already here)
REM   2. Apply nuked-sc55-uart-routing.patch
REM   3. Build with CMake + MinGW + SDL2
REM   4. Copy nuked-sc55.exe, SDL2.dll and the MinGW runtime into ..\NukedSC55
REM
REM Requirements (Windows):
REM   - Git, CMake
REM   - MinGW toolchain. By default Qt's is used (same as jmp):
REM       set MINGW_DIR=C:\Qt\Tools\mingw1310_64        (override if elsewhere)
REM   - SDL2 MinGW development package. Point SDL2_ROOT at the UNZIPPED folder
REM     that contains x86_64-w64-mingw32\ :
REM       set SDL2_ROOT=C:\path\to\SDL2-2.30.12
REM     Get it from https://github.com/libsdl-org/SDL/releases (…-mingw.zip).
REM ========================================

REM %~dp0 ends with a backslash; "%~dp0" then reads as ...\" and the trailing
REM backslash escapes the closing quote for tools like tar. HERE has it stripped.
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"

set SRC_DIR=%~dp0Nuked-SC55-GUI-Float
set BUILD_DIR=%SRC_DIR%\build
set OUT_DIR=%~dp0..\NukedSC55

if not defined MINGW_DIR set MINGW_DIR=C:\Qt\Tools\mingw1310_64
set PATH=%MINGW_DIR%\bin;%PATH%

echo.
echo ========================================
echo  Patched Nuked-SC55 build
echo ========================================
echo   Source : %SRC_DIR%
echo   Output : %OUT_DIR%
echo   MinGW  : %MINGW_DIR%
echo.

REM ---- toolchain checks -------------------------------------------------
where git >nul 2>nul || (echo [ERROR] git not found in PATH. & goto :fail)
where cmake >nul 2>nul || (echo [ERROR] cmake not found in PATH. & goto :fail)
if not exist "%MINGW_DIR%\bin\g++.exe" (
    echo [ERROR] MinGW g++ not found at %MINGW_DIR%\bin
    echo         Set MINGW_DIR to your MinGW toolchain and run again.
    goto :fail
)

REM ---- SDL2 ------------------------------------------------------------
REM Order: honour a caller-set SDL2_ROOT, else reuse an SDL2-* folder already
REM sitting next to this script, else download the dev package automatically.
if not defined SDL2_ROOT (
    for /d %%D in ("%~dp0SDL2-*") do set "SDL2_ROOT=%%D"
)
if not defined SDL2_ROOT (
    set "SDL2_VER=2.30.12"
    set "SDL2_ZIP=%~dp0SDL2-devel-!SDL2_VER!-mingw.zip"
    echo SDL2 not found - downloading the MinGW dev package ^(!SDL2_VER!^) ...
    curl -L -f -o "!SDL2_ZIP!" "https://github.com/libsdl-org/SDL/releases/download/release-!SDL2_VER!/SDL2-devel-!SDL2_VER!-mingw.zip"
    if errorlevel 1 (
        echo [ERROR] SDL2 download failed. Check your connection, or download
        echo         SDL2-devel-!SDL2_VER!-mingw.zip yourself, unzip it, and:
        echo             set SDL2_ROOT=C:\path\to\SDL2-!SDL2_VER!
        goto :fail
    )
    echo Extracting ...
    tar -xf "!SDL2_ZIP!" -C "%HERE%"
    if errorlevel 1 (
        echo [ERROR] Could not unzip SDL2 ^(tar not available?^). Unzip
        echo         "!SDL2_ZIP!" here by hand and run this script again.
        goto :fail
    )
    del "!SDL2_ZIP!" >nul 2>nul
    for /d %%D in ("%~dp0SDL2-*") do set "SDL2_ROOT=%%D"
)
if not defined SDL2_ROOT (
    echo [ERROR] SDL2 still not found after download.
    goto :fail
)
set SDL2_CMAKE=%SDL2_ROOT%\x86_64-w64-mingw32\lib\cmake\SDL2
set SDL2_BIN=%SDL2_ROOT%\x86_64-w64-mingw32\bin
if not exist "%SDL2_CMAKE%\SDL2Config.cmake" if not exist "%SDL2_CMAKE%\sdl2-config.cmake" (
    echo [ERROR] SDL2 cmake config not under: %SDL2_CMAKE%
    echo         SDL2_ROOT should be the folder holding x86_64-w64-mingw32\ .
    goto :fail
)
echo   SDL2   : %SDL2_ROOT%
echo.

REM ---- 1. source ------------------------------------------------------
if not exist "%SRC_DIR%\CMakeLists.txt" (
    echo [1/4] Cloning Nuked-SC55-GUI-Float ...
    git clone https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float.git "%SRC_DIR%" || goto :fail
) else (
    echo [1/4] Source already present - skipping clone.
)

REM ---- 2. patch -------------------------------------------------------
echo [2/4] Applying patch ...
cd /d "%SRC_DIR%"
git apply --check "%~dp0nuked-sc55-uart-routing.patch" 2>nul
if errorlevel 1 (
    git apply --reverse --check "%~dp0nuked-sc55-uart-routing.patch" 2>nul
    if errorlevel 1 (
        echo [ERROR] Patch does not apply cleanly. The upstream source may have
        echo         moved on. Try a fresh clone, or re-generate the patch.
        cd /d "%~dp0"
        goto :fail
    ) else (
        echo        Patch already applied - skipping.
    )
) else (
    git apply "%~dp0nuked-sc55-uart-routing.patch" || goto :fail
    echo        Patch applied.
)
cd /d "%~dp0"

REM ---- 3. build -------------------------------------------------------
echo [3/4] Building ...
set "SDL2_CMAKE_FS=%SDL2_CMAKE:\=/%"
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G "MinGW Makefiles" ^
          -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR="%SDL2_CMAKE_FS%" || goto :fail
)
cmake --build "%BUILD_DIR%" -j 8 || goto :fail

REM ---- 4. install into jmp's NukedSC55 --------------------------------
echo [4/4] Installing into %OUT_DIR% ...
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
copy /y "%BUILD_DIR%\nuked-sc55.exe" "%OUT_DIR%\" >nul || goto :fail
copy /y "%SDL2_BIN%\SDL2.dll" "%OUT_DIR%\" >nul
for %%D in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if exist "%MINGW_DIR%\bin\%%D" copy /y "%MINGW_DIR%\bin\%%D" "%OUT_DIR%\" >nul
)
if exist "%SRC_DIR%\data\sc55_background.bmp" copy /y "%SRC_DIR%\data\sc55_background.bmp" "%OUT_DIR%\" >nul

echo.
echo ========================================
echo  DONE
echo ========================================
echo.
echo Installed to: %OUT_DIR%
dir /b "%OUT_DIR%"
echo.
echo Still needed there: your SC-55 ROM files (rom1.bin, rom2.bin, rom_sm.bin,
echo waverom1.bin, waverom2.bin - or the sc55_*.bin naming). They are Roland's,
echo so you must supply them yourself. Any model is auto-detected.
echo.
if not defined NO_PAUSE pause
exit /b 0

:fail
echo.
echo Build did not complete.
if not defined NO_PAUSE pause
exit /b 1
