Put your MT-32 / CM-32L ROMs in this folder
================================================================

One control ROM and one PCM ROM, as a pair:

  MT-32     MT32_CONTROL.ROM   +  MT32_PCM.ROM
  CM-32L    CM32L_CONTROL.ROM  +  CM32L_PCM.ROM

Filenames do not matter - the ROMs are identified by their content,
so whatever your dump is called will work. Several pairs can sit
here together and you can pick between them in the MT-32 window.

Then choose [MT32 JMP] in the output device list.

Split dumps (upper half / lower half) are ignored - use merged files.

The ROMs are Roland's and are not included with jmp. The emulator
itself is munt (mt32emu, LGPL-2.1) and ships with jmp, so there is
nothing to build.   https://github.com/munt/munt
