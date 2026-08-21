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

## Three changes to get sound, and six for timing and cost

Routing alone produces **silence**. Two more things are tied to the computer
switch and have to come loose with it. The first three are needed for any sound
at all; any one alone is still silent. (4), (6) and (8) fix the timing of what
arrives, (5) fixes what it costs to run, (7) keeps the audio buffer from running
dry when the machine is busy, and **(9) is the one that cured the crackling
reported from a laptop** — the panel renderer, of all things.

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
   delivers everything buffered, idling only when the line is quiet. The stock
   loop additionally spun at full speed **holding `serial_io_mutex`**, starving
   the update it depended on and burning a core.

5. **That idle wait must not go below 1 ms** (2026-08-20). MinGW's
   `std::this_thread::sleep_for` spins instead of sleeping for sub-millisecond
   waits, so the 250 us this patch originally used did not idle at all - it
   burned a whole core, exactly like the stock loop it replaced.

   Measured by driving the emulator with an identical generated note stream
   (4 channels, a note every 60 ms, ~32 voices sounding) over both transports,
   on the same machine, 100 % = one core:

   | binary | transport | CPU |
   |---|---|---|
   | stock 0.6.3 | loopMIDI | 28.2 % |
   | patched, 250 us | loopMIDI | 28.7 % |
   | stock 0.6.3 | named pipe | 128.7 % |
   | patched, 250 us | named pipe | 128.2 / 128.5 / 129.0 % |
   | patched, **1 ms** | named pipe | 27.1 / 28.5 / 30.0 % |

   The same binary is 28.7 % over loopMIDI and 128 % over the pipe, so the
   100-point gap is this one line and nothing else. The stock build measures the
   same 128 % over the pipe, so this is upstream's defect, not one the patch
   introduced. Note count barely moves any of these figures - the H8 is
   interpreted cycle-accurately, so the cost is flat.

   **1 ms costs nothing in latency.** The emulated UART paces arriving bytes
   itself: `mcu.uart_rx_delay = mcu.cycles + 3000` at a 24 MHz cycle base is
   125 us between bytes, so only the first byte of a burst can wait at all, and
   it waits a uniform amount rather than jittering. There is no trade-off here
   to expose as a setting - below 1 ms buys no latency, only a burnt core.

   An MMCSS registration (`AvSetMmThreadCharacteristicsW`, "Pro Audio") was
   added on 2026-08-19 on the theory that the noise was lock contention, and
   **removed again on 2026-08-20** when the measurements above showed the cause
   was the spin. It made no measurable difference — the audio path never takes
   `serial_io_mutex`. The idea was not wrong, it was on the wrong thread; see
   (7).

6. **The reader waits on the read event instead of polling** (2026-08-20). A
   MIDI port delivers by callback: the thread is woken when a byte arrives and
   costs nothing in between. This path polled, and that was the last structural
   difference left between it and the port path that plays cleanly.

   `SERIAL_Update()` has just issued the next overlapped read when the loop
   reaches the wait, so `olRead.hEvent` is unsignalled until a byte actually
   lands — the thread parks outright. The 50 ms timeout exists only so it
   notices `serial_thread_run` going false, and so a closed port degrades into
   a slow poll rather than a hang. The handle is **duplicated** rather than
   borrowed, because `SerialClose()` can close the original from the event-loop
   thread while the reader is parked on it, and waiting on a handle someone
   else closed is undefined.

   Measured: the same sender pushed **198 messages in 15 s while polling and
   233 with the wait** — writes are accepted as fast as the reader takes them.
   A 512 KB burst of real note messages drains without the write ever blocking,
   which is the failure mode to rule out ("never wakes" is invisible in a CPU
   figure).

7. **The audio producer thread asks MMCSS for "Pro Audio"** (2026-08-20), in
   `FE_RunInstanceSDL`. This is where the priority idea from (5) belonged.

   That loop keeps `buffer_count * buffer_size` frames queued and no more —
   16 × 512 at 66207 Hz is **124 ms** — then sleeps a millisecond at a time.
   So the entire margin against a dropout is 124 ms of scheduling luck. SDL
   registers the audio *callback* thread with MMCSS, but the callback has
   nothing to hand out if the producer has not run, and the producer was left
   at ordinary priority.

   Measured with the pipe connected, the busiest thread — the one accumulating
   the CPU time, i.e. the producer — went from priority **10 (base 8) to 25**,
   above SDL's own callback at 24. It costs nothing while the buffer is full,
   since the thread sleeps then and MMCSS returns the boost when it idles.
   Needs `avrt` on the link line, which the patch adds.

   If it is not enough, the other dial is the buffer itself: `-b 512:32`
   doubles the margin to 247 ms, at the cost of the same amount of latency.

8. **The serial reader thread also asks MMCSS for "Pro Audio"** (2026-08-20).
   This is the same call that was added on 2026-08-19 and removed the next day;
   it is back for a different and better-grounded reason, and the earlier
   justification stays wrong.

   The owner's testing is what turned it around. **The same emulator binary,
   launched by hand and driven through loopMIDI, plays cleanly while a browser
   loads; driven by jmp through the pipe it crackles.** That rules out the
   build, SDL, the ROMs and the machine, and leaves how the bytes get in:

   | route | thread hops between "jmp decides to send" and `MCU_PostUART` |
   |---|---|
   | MIDI port | **0** — `midiOutShortMsg` enters the kernel and Windows' own MIDI stack delivers on a realtime thread |
   | named pipe | **2** — jmp's sender thread, then this one |

   Both hops were at priority 8-10, so both are held up exactly when the
   machine is busy, and the port route has nothing that can be. jmp raises its
   sender thread the same way (`sc55bridge.cpp`).

   Verified in place: while playing, this thread sits at priority **25 with
   0.00 s of CPU** — parked on the read event from (6), costing nothing, but
   scheduled the moment a byte lands.

   **Not verified against the symptom.** Neither is (7). The crackle has never
   been reproducible on the development machine, a 12-core desktop.

9. **The panel renderer: ask for acceleration, filter linearly, redraw half as
   often** (2026-08-20). **This is the one that fixed the crackling**, after
   (5) through (8) did not.

   Stock built the panel renderer as `SDL_CreateRenderer(m_window, -1, 0)` -
   no `SDL_RENDERER_ACCELERATED` - and set `RENDER_SCALE_QUALITY` to `"BEST"`.
   With jmp's `--lcd-scale 0.75`, every frame re-filtered the panel artwork,
   66 times a second.

   On the reporter's i5-8250U that was enough to make the emulator lose
   realtime: the music slowed and crackled whenever a browser started. **It
   never showed as CPU** - idle measured 58.3 % at scale 0.75 and 58.1 % at
   1.0 - because the work landed on the integrated GPU, which shares its power
   budget with the CPU cores. Running at 1.0, which skips the rescale
   altogether, was clean; that was the first thing he heard fix it.

   The renderer turned out to be `direct3d` on his machine all along, so the
   software fallback was **not** the cause: `"BEST"` on D3D9 means *anisotropic*
   filtering, which takes many texture samples per output pixel. That is meant
   for surfaces viewed at an angle, not for a flat downscale. `"linear"` is
   visually indistinguishable here and enormously cheaper. Redrawing was also
   halved to about 33 fps - the loop ticks at 15 ms to poll events, not because
   a panel that barely changes needs drawing that often.

   With all three, **0.75 is fine on that laptop** (confirmed 2026-08-20). The
   acceleration request stays anyway, since falling back silently is exactly
   the kind of thing that should not be invisible - which is why the renderer's
   name now goes in the **window title**, `Nuked SC-55: SC-55mk2  [direct3d]`.
   It has to be the title: this links `SDL2main`, so the binary is Windows
   GUI-subsystem and never attaches to the console it was started from -
   `printf` has nowhere to go, and even `> log.txt 2>&1` came back empty.

### What did not fix it, and is kept anyway

(6) the event wait, (7) and (8) the MMCSS registrations. None of them changed
what he heard. They are still right - polling where the other transport uses a
callback was a real difference, and an audio producer at priority 8 was a real
hazard - but **the report was never about scheduling**, and four rounds went
into it on reasoning before anyone measured the thing that mattered. The
sequence that actually worked was: measure both transports side by side, get
the owner to run the same emulator by hand (which cleared the binary, SDL, the
ROMs and the machine at a stroke), and then look at what was left in the
command line.

### SDL is shipped newer than it is built against

Not part of the patch, but it belongs with this history: the stock
distribution ships **SDL 2.32.10** and jmp's `NukedSC55` bundled **2.30.12**.
The emulator opens its device at 66207 Hz, which no card runs natively, so SDL
resamples every buffer. Swapping the runtime to 2.32.10 was a measurable
improvement on the laptop (owner, 2026-08-20). `build_local_full.bat` now
prefers `sc55\sdl2\runtime\SDL2.dll` when it exists — it used to copy the dev
package's DLL and silently undo the swap on every build.

A tenth, unrelated hunk qualifies `isnan` as `std::isnan` in `lcd_sdl.cpp`;
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
