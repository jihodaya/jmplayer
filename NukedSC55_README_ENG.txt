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
