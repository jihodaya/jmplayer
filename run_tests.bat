@echo off
REM ========================================
REM CORPUS TESTS - development only, never shipped
REM ========================================
REM
REM Builds the four test tools into build_tests\ and runs them. Nothing here
REM touches build\, release\ or the player: they are separate executables behind
REM -DBUILD_TESTS=ON, which build_release.bat never passes.
REM
REM   inputs    rubbish into every value that comes from outside the program
REM   loadall   opens every song in the library through jmp's own loaders
REM   midiall   MidiPlayer's parse, tempo, sound-mode verdict and lyric decoding
REM   render    renders a chosen set through the real players and hashes the audio
REM
REM They run cheapest-first, so a broken build is reported in seconds rather than
REM after a full library walk.
REM
REM Why a local script and not CI: the only test material worth having is the
REM library itself, and it cannot be redistributed. This has to be something a
REM person runs deliberately after changing something.
REM
REM   run_tests.bat                 REM check against the recorded baselines
REM   run_tests.bat rebaseline      REM accept today's results as the new baselines
REM   run_tests.bat quick           REM inputs + render only, skip the library walk
REM   run_tests.bat nopause
REM
REM Set LIBRARY if the songs are not on D:\mt32.
REM ========================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

if "%QT_DIR%"==""    set "QT_DIR=C:\Qt\6.9.2\mingw_64"
if "%MINGW_DIR%"=="" set "MINGW_DIR=C:\Qt\Tools\mingw1310_64"
if "%LIBRARY%"==""   set "LIBRARY=D:\mt32"

set "MINGW_DIR_FS=%MINGW_DIR:\=/%"
set "PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%"

set "BASE_LOAD=tests\baseline-loadall.txt"
set "BASE_RENDER=tests\baseline-render.txt"
set "SONGS=tests\songs.txt"
set "BASE_MIDI=tests\baseline-midiall.txt"
set "BASE_MIDI_MID=tests\baseline-midiall-mid.txt"

REM 116,000 .mid would take half an hour; every 20th takes a minute and has
REM been enough so far. The stride is recorded in the baseline's first line, so
REM a run with a different one is refused rather than compared wrongly.
if "%MID_STRIDE%"=="" set "MID_STRIDE=20"

set "REBASE=0"
set "QUICK=0"
set "NOPAUSE=0"
for %%a in (%*) do (
    if /I "%%a"=="rebaseline" set "REBASE=1"
    if /I "%%a"=="quick"      set "QUICK=1"
    if /I "%%a"=="nopause"    set "NOPAUSE=1"
)

echo.
echo ========================================
echo CORPUS TESTS
echo ========================================
echo   library  : %LIBRARY%
if "%QUICK%"=="1" echo   mode     : quick ^(skipping the library walk^)
if "%REBASE%"=="1" echo   mode     : recording new baselines
echo.

echo [1/5] Configuring...
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON ^
      -DCMAKE_C_COMPILER="%MINGW_DIR_FS%/bin/gcc.exe" ^
      -DCMAKE_CXX_COMPILER="%MINGW_DIR_FS%/bin/g++.exe" ^
      -DCMAKE_PREFIX_PATH="%QT_DIR:\=/%" ^
      -S . -B build_tests > build_tests_configure.log 2>&1
if errorlevel 1 (
    echo ERROR: configure failed - see build_tests_configure.log
    goto :fail
)

echo [2/5] Building the tools...
cmake --build build_tests --target inputs loadall midiall render mt32 mt32ui -j 4 > build_tests_build.log 2>&1
if errorlevel 1 (
    echo ERROR: build failed - see build_tests_build.log
    goto :fail
)

set "TOTAL=0"

echo.
echo ----------------------------------------
echo [3/5] inputs   - external values
echo ----------------------------------------
build_tests\inputs.exe
if errorlevel 1 set /a TOTAL+=1

echo.
echo ----------------------------------------
echo mt32     - the MT-32 engine, no window
echo ----------------------------------------
REM Before the quick-mode jump on purpose: this one never touches the library,
REM so there is nothing for "quick" to save by skipping it.
REM
REM Skipped without ROMs, which is the normal case - they are Roland's and are
REM not in this tree. Set MT32_ROMS to a folder holding a control ROM and a PCM
REM ROM to include it.
if "%MT32_ROMS%"=="" (
    echo   SKIPPED: set MT32_ROMS to a folder of MT-32 / CM-32L ROMs to run this
) else (
    build_tests\mt32.exe --roms "%MT32_ROMS%" --seconds 2
    if errorlevel 1 set /a TOTAL+=1

    echo.
    echo   the panel and its ROM selector ^(a window appears briefly^)
    build_tests\mt32ui.exe --roms "%MT32_ROMS%"
    if errorlevel 1 set /a TOTAL+=1
)

if "%QUICK%"=="1" goto :render

echo.
echo ----------------------------------------
echo loadall  - every song in the library
echo ----------------------------------------
if "%REBASE%"=="1" if exist "%BASE_LOAD%" move /y "%BASE_LOAD%" "%BASE_LOAD%.prev" >nul
if not exist "%LIBRARY%" (
    echo   SKIPPED: library folder not found: %LIBRARY%
    echo            Set LIBRARY to where the songs are, e.g.  set LIBRARY=E:\music
) else (
    build_tests\loadall.exe "%LIBRARY%" --baseline "%BASE_LOAD%" --quiet
    if errorlevel 1 set /a TOTAL+=1
)

echo.
echo ----------------------------------------
echo [4/5] midiall  - MidiPlayer's parsing
echo ----------------------------------------
if "%REBASE%"=="1" (
    if exist "%BASE_MIDI%"     move /y "%BASE_MIDI%"     "%BASE_MIDI%.prev" >nul
    if exist "%BASE_MIDI_MID%" move /y "%BASE_MIDI_MID%" "%BASE_MIDI_MID%.prev" >nul
)
if not exist "%LIBRARY%" (
    echo   SKIPPED: library folder not found
) else (
    echo   containers ^(.nob .okm .okw^) - all of them
    build_tests\midiall.exe "%LIBRARY%" --formats nob,okm,okw --baseline "%BASE_MIDI%" --quiet
    if errorlevel 1 set /a TOTAL+=1
    echo.
    echo   .mid - every %MID_STRIDE%th file
    build_tests\midiall.exe "%LIBRARY%" --formats mid,midi --stride %MID_STRIDE% --baseline "%BASE_MIDI_MID%" --quiet
    if errorlevel 1 set /a TOTAL+=1
)

:render
echo.
echo ----------------------------------------
echo [5/5] render   - audio of the chosen songs
echo ----------------------------------------
if "%REBASE%"=="1" if exist "%BASE_RENDER%" move /y "%BASE_RENDER%" "%BASE_RENDER%.prev" >nul
if not exist "%SONGS%" (
    echo   SKIPPED: no song list at %SONGS%
) else (
    build_tests\render.exe "%SONGS%" --seconds 15 --baseline "%BASE_RENDER%"
    if errorlevel 1 set /a TOTAL+=1
)

echo.
if "%TOTAL%"=="0" (
    echo ========================================
    echo ALL CLEAR
    echo ========================================
) else (
    echo ========================================
    echo %TOTAL% TOOL^(S^) REPORTED A PROBLEM
    echo ========================================
    echo.
    echo If the change was intended, accept it with:  run_tests.bat rebaseline
    echo To hear what moved in the audio:             build_tests\render.exe %SONGS% --write-wav wav_out
)

if "%NOPAUSE%"=="0" pause
exit /b %TOTAL%

:fail
if "%NOPAUSE%"=="0" pause
exit /b 2
