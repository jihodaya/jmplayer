# Install script for directory: D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/MidiPlayer")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Qt/Tools/mingw1310_64/bin/objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/adplug" TYPE FILE FILES
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/adplug.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/emuopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/fmopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/silentopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/opl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/diskopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/depack.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/sixdepack.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/ungzip.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/unlzh.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/unlzss.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/unlzw.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/a2m.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/a2m-v2.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/amd.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/bam.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/d00.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/dfm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/hsc.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/hsp.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/imf.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/ksm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/lds.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mid.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mkj.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mtr.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mtk.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/pis.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/protrack.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/rad2.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/raw.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/s3m.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/sa2.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/sng.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/u6m.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/player.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/plx.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/fmc.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mad.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/xad.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/bmf.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/flash.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/hyp.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/psi.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/rat.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/hybrid.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/rol.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/adtrack.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/cff.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/dtm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/dmo.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/fprovide.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/database.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/players.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/xsm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/adlibemu.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/kemuopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/dro.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/realopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/analopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/temuopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/msc.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/rix.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/adl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/jbm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/cmf.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/surroundopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/dro2.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/got.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/wemuopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/woodyopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/nemuopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/nukedopl.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mus.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/mdi.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/cmfmcsop.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/coktel.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/composer.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/vgm.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/sop.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/herad.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/strnlen.h"
    "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-src/src/load_helper.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/py/midi-k-c260415/github_clean_dist/build/_deps/adplug-build/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
