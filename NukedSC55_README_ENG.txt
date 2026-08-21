Put Nuked-SC55 in this folder
================================================================

jmp launches nuked-sc55.exe from this folder and sends MIDI to it
directly. No virtual MIDI cable such as loopMIDI is needed.

What goes here
  nuked-sc55.exe        (see "Important" below - it must be patched)
  SDL2.dll
  ROM files             (rom1.bin, rom2.bin ... or sc55_rom1.bin ...)
  sc55_background.bmp   (without it the window is a plain LCD)

Any ROM set works. SC-55, SC-55mk2 and the rest are detected
automatically - just drop them in.


Important - a stock build will NOT work
----------------------------------------------------------------
The released Nuked-SC55 does not open the channel jmp connects on,
so you get no sound at all.

Build it yourself with the patch shipped alongside jmp.
Patch and build instructions:  emulator-patch folder

The patch delivers incoming MIDI to the main MCU's MIDI input rather
than the sub-MCU's serial receiver. It also adds window scaling and
improves ROM detection.


Settings (settings.ini)
----------------------------------------------------------------
Only add what you need; anything absent uses the default.

  [Sc55]
  WindowScale=0.75      SC-55 window scale (0.25 - 4.0). The panel
                        artwork is a fixed 1120x233, which crowds the
                        screen, so it is shrunk by default.
                        1.0 skips the scaling altogether.

  HidePanel=false       true runs the emulator with --no-lcd, so no
                        window appears at all. You lose the display
                        and the cost of drawing it.

  AudioBuffer=          Emulator audio buffer, written "size:count",
                        e.g. 512:32. Empty means the emulator's own
                        default (512:16, about 124 ms).
                        Larger survives interruptions better but the
                        sound arrives later by the same amount -
                        512:32 means 247 ms, enough for lyrics to
                        visibly lead the music.

  SerialBytesPerSecond= Rate limit on the channel. Unlimited by
                        default. Only lower it (to around 1000) if
                        you are forcing an unpatched emulator to work.


If the sound breaks up or crackles
----------------------------------------------------------------
On a low-powered laptop, starting something like a web browser can
produce a burst of crackling and a slight drag in tempo.

First look at the SC-55 window's title bar. This is what you want:

  Nuked SC-55: SC-55mk2  [direct3d]

If the brackets say this instead, graphics acceleration is not being
used and the panel is being drawn entirely on the CPU:

  Nuked SC-55: SC-55mk2  [SOFTWARE: software]

Check your graphics driver in that case. If it cannot be fixed,
WindowScale=1.0 removes the scaling work altogether.

Then try these in order:

  1) WindowScale=1.0
  2) HidePanel=true
  3) AudioBuffer=512:32   (only if you can live with the lyric lag)

Note that the emulator imitates the SC-55's CPU cycle by cycle, so
there is no way to make it do less arithmetic. Every option above
works by taking something else away from around it.


About ROMs
----------------------------------------------------------------
The ROM files are Roland's copyrighted material. They are not
shipped with jmp - you must obtain them yourself.


Copyright / licence
----------------------------------------------------------------
Nuked-SC55 is not part of jmp. It is a separate program.

  Original  Nuked-SC55 - nukeykt
            https://github.com/nukeykt/Nuked-SC55
  GUI fork  Nuked-SC55-GUI-Float - linoshkmalayil
            https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float
  Licence   MAME License (non-commercial use only)

jmp is MIT-licensed; Nuked-SC55 is under the MAME licence
(non-commercial). Their terms differ, so jmp ships only this
folder and its note, never the emulator.
