이 폴더에 Nuked-SC55를 넣으세요
Put Nuked-SC55 in this folder
================================================================

jmp는 이 폴더의 nuked-sc55.exe 를 직접 실행해서 MIDI를 보냅니다.
loopMIDI 같은 가상 MIDI 케이블이 필요 없습니다.

넣을 것
  nuked-sc55.exe        (아래 "중요" 참고 - 수정본이어야 합니다)
  SDL2.dll
  ROM 파일들            (rom1.bin, rom2.bin ... 또는 sc55_rom1.bin ...)
  sc55_background.bmp   (없으면 화면이 단순해집니다)

ROM은 어떤 세트든 됩니다. SC-55, SC-55mk2 등 넣기만 하면 알아서 판별합니다.


중요 - 원본 빌드로는 동작하지 않습니다
----------------------------------------------------------------
배포되는 원본 Nuked-SC55는 jmp가 여는 통로를 열지 않습니다.
소리가 전혀 나지 않습니다.

jmp와 함께 배포되는 패치를 적용해 직접 빌드해야 합니다.
패치와 빌드 방법:  emulator-patch 폴더

패치가 하는 일은 받은 MIDI를 보조 MCU의 시리얼 수신부가 아니라
메인 MCU의 MIDI 입력으로 전달하는 것입니다. 그밖에 창 크기 조절과
ROM 자동 판별 보완이 들어 있습니다.


ROM에 대하여
----------------------------------------------------------------
ROM 파일은 롤랜드의 저작물입니다. jmp와 함께 배포하지 않으며,
직접 구하셔야 합니다.


저작권 / 라이선스
----------------------------------------------------------------
Nuked-SC55 는 jmp의 일부가 아닙니다. 별개의 프로그램입니다.

  원본      Nuked-SC55 - nukeykt
            https://github.com/nukeykt/Nuked-SC55
  GUI 포크  Nuked-SC55-GUI-Float - linoshkmalayil
            https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float
  라이선스  MAME License (비상업적 사용에 한함)

jmp는 MIT 라이선스이고, Nuked-SC55는 MAME 라이선스(비상업)입니다.
서로 조건이 달라 jmp는 에뮬레이터를 함께 배포하지 않고, 이 폴더만 만들어 둡니다.
