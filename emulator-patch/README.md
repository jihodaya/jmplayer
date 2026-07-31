# Nuked-SC55 UART routing patch

> **Nuked-SC55 is not part of jmp, and this patch is not Nuked-SC55.**
>
> | | |
> |---|---|
> | Original | [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55) — nukeykt |
> | GUI fork this patches | [Nuked-SC55-GUI-Float](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float) — linoshkmalayil |
> | Their licence | MAME License — **non-commercial use only** |
>
> The patch file necessarily quotes a small amount of their code as context.
> Everything else here is jmp's own work, under jmp's MIT licence.
> Applying the patch produces a derivative of their work, which stays under
> **their** licence — so a build made this way must not be sold or used
> commercially, and if you pass one on you must include its complete source.

`nuked-sc55-uart-routing.patch` changes where Nuked-SC55 delivers bytes that
arrive on its serial port.

## Why

jmp drives the emulator over a named pipe so no virtual MIDI cable is needed.
The emulator accepts that pipe as its *serial* port, and stock builds hand those
bytes to the sub-MCU's emulated RS-232 receiver (`FE_SendSerial` →
`Emulator::PostSerial` → `SM_PostSerial`).

That receiver is a poor carrier for a host that is sequencing in real time:

* it takes one byte per poll with a fixed inter-byte delay, imitating a
  31250-baud line;
* its 1024-byte buffer has **no overflow check** — `SM_PostSerial` advances the
  write pointer regardless of the read pointer, so anything the firmware has not
  consumed yet is silently overwritten.

The result was wrong instruments, stuck notes and unsteady tempo. Pacing the
writes from jmp's side did not cure it (measured 2026-07-29); the transport
itself was wrong.

The patch routes those bytes to `Emulator::PostMIDI` → `MCU_PostUART` — the main
MCU's UART, which is the entry point a MIDI input reaches, and the reason the
same music played correctly through loopMIDI. Its buffer is 8192 bytes and is
drained per instruction, so no pacing is needed.

## Three changes, not one

Routing alone produces **silence**. Two more things are tied to the computer
switch and have to come loose with it. All three are needed; any one alone is
still silent.

1. **`FE_RouteSerial`** delivers to `PostMIDI` instead of `PostSerial`.

2. **The serial port opens even with the switch at MIDI.** `-st RS232C_1` is not
   a wiring choice — it turns the rear COMPUTER switch to PC-1, and `MCU_ReadAnalog`
   reports that position to the firmware. In the PC-1/PC-2/Mac positions a real
   SC-55mk2 reads the serial port and **ignores MIDI IN**, so the routed bytes
   were being thrown away by the firmware itself. The switch has to stay at MIDI,
   which stock builds refuse to combine with `-sp`.

   The romset restriction goes too: the emulated RS-232 hardware is not involved
   on this path, so the pipe is not mk2/st-only.

3. **`SERIAL_Update()` runs whenever a port is open.** `FE_EventLoop` called it
   only when the switch was in a serial position, and it is the sole place the
   overlapped read is issued — with the switch at MIDI, `SERIAL_HasData()` stayed
   false forever and the reader thread never saw a byte. `FE_Run`'s flag now
   means "a port is open" rather than "the switch is serial".

4. **`SERIAL_Read_Updater` pumps the port itself.** With (3) alone the sound is
   correct but *drags in fast passages*: `FE_EventLoop` runs on `SDL_Delay(15)`,
   so the host's bytes were collected in ~15 ms clumps instead of arriving as
   sent. The reader thread now calls `SERIAL_Update()` in its own loop and
   delivers everything buffered, idling 250 us only when the line is quiet. The
   old loop additionally spun at full speed **holding `serial_io_mutex`**,
   starving the update it depended on and burning a core.

A fourth, unrelated hunk qualifies `isnan` as `std::isnan` in `lcd_sdl.cpp`;
plain `isnan` is only in scope when `<math.h>` leaks it as a macro, which the
MinGW toolchain used here does not do.

## Note for anyone debugging this further

On the SC-55mk2 the **sub-MCU** drains the main MCU's MIDI buffer
(`submcu.cpp`), not `MCU_UpdateUART_RX` — `MCU_Step` only calls that on
mk1/JV880/SCB55. Instrumenting `MCU_UpdateUART_RX` on an mk2 romset therefore
looks like "the UART is never serviced" when nothing is wrong.

## The easy way — `build_patched_sc55.bat`

On Windows, `build_patched_sc55.bat` (in this folder) does the whole thing:
clones the source, applies the patch, builds it, and copies the result into
jmp's `NukedSC55` folder. **SDL2 is downloaded automatically** the first time,
so usually you just run it:

```bat
build_patched_sc55.bat
```

You still need **Git**, **CMake** and a **MinGW C++23 toolchain** installed.
Qt's MinGW is used by default; point `MINGW_DIR` at yours if it lives elsewhere:

```bat
set MINGW_DIR=C:\Qt\Tools\mingw1310_64
build_patched_sc55.bat
```

To skip the automatic download, set `SDL2_ROOT` to an already-unzipped SDL2
MinGW dev package (the folder holding `x86_64-w64-mingw32\`).

Re-running it is safe: it reuses the download, skips the clone/patch if already
done, and just rebuilds. Afterwards, add your SC-55 ROM files to `NukedSC55\`
(see below).

## The manual way

```
git clone https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float.git
cd Nuked-SC55-GUI-Float
git apply /path/to/nuked-sc55-uart-routing.patch

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
      -DSDL2_DIR=<sdl2>/x86_64-w64-mingw32/lib/cmake/SDL2
cmake --build build -j 8
```

Needs a C++23 compiler and the SDL2 MinGW development package. `rtmidi` is not
required on Windows.

Put `nuked-sc55.exe` in jmp's `NukedSC55` folder alongside `SDL2.dll`, the ROM
files, and — for a MinGW build — `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and
`libwinpthread-1.dll`.

**Note:** apply the patch with an unmodified checkout. The `.patch` file uses LF
line endings; if your tools rewrite it to CRLF, `git apply` will reject it.

## Licence

**Not for redistribution with jmp.** Nuked-SC55 is under the MAME licence
(non-commercial); jmp is MIT. Shipping a build of the emulator would drag jmp's
distribution under the stricter non-commercial terms, so only this patch - jmp's
own work - lives here. The emulator and its ROMs are the user's to obtain.

## Using an unpatched emulator

Still possible, just worse. Lower jmp's send rate so the sub-MCU's small buffer
is not overrun — around 1000 was the best that path managed:

```
[Sc55]
SerialBytesPerSecond=1000
```
