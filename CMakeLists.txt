cmake_minimum_required(VERSION 3.16)

project(MidiPlayer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network)

# Qt6 automatic MOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# External Libraries (FetchContent)
include(FetchContent)

# 1. libbinio (AdPlug dependency)
FetchContent_Declare(
    libbinio
    GIT_REPOSITORY https://github.com/adplug/libbinio.git
    GIT_TAG master
)
# Disable libbinio tests and examples if possible
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libbinio)

# 2. adplug
FetchContent_Declare(
    adplug
    GIT_REPOSITORY https://github.com/adplug/adplug.git
    GIT_TAG master
)
# Disable adplug tests/examples to speed up build
set(BUILD_ADPLAY OFF CACHE BOOL "" FORCE)
set(BUILD_ADPLUG_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(adplug)

# Patch adplug's mus.h and mus.cpp to expose current_inst and instrument getter
set(MUS_H_PATH "${adplug_SOURCE_DIR}/src/mus.h")
set(MUS_CPP_PATH "${adplug_SOURCE_DIR}/src/mus.cpp")

if(EXISTS ${MUS_H_PATH})
    file(READ ${MUS_H_PATH} MUS_H_CONTENT)
    string(FIND "${MUS_H_CONTENT}" "current_inst" HAS_CURRENT_INST)
    if(HAS_CURRENT_INST EQUAL -1)
        string(REPLACE "uint8_t\t\tvolume[MAX_VOICES];" "uint8_t\t\tvolume[MAX_VOICES];\n\tuint8_t\t\tcurrent_inst[MAX_VOICES];" MUS_H_CONTENT "${MUS_H_CONTENT}")
    endif()
    string(FIND "${MUS_H_CONTENT}" "getvoiceinstrument" HAS_MUS_GETTER)
    if(HAS_MUS_GETTER EQUAL -1)
        string(REPLACE "private:" "std::string getvoiceinstrument(unsigned int voice) {\n\t\tif (voice >= MAX_VOICES) return std::string();\n\t\treturn current_inst[voice] < nrTimbre ? std::string(insts[current_inst[voice]].name) : std::string();\n\t};\n\tunsigned int getnumvoices() { return MAX_VOICES; };\n\nprivate:" MUS_H_CONTENT "${MUS_H_CONTENT}")
    endif()
    file(WRITE ${MUS_H_PATH} "${MUS_H_CONTENT}")
endif()

if(EXISTS ${MUS_CPP_PATH})
    file(READ ${MUS_CPP_PATH} MUS_CPP_CONTENT)
    string(FIND "${MUS_CPP_CONTENT}" "current_inst" HAS_CURRENT_INST)
    if(HAS_CURRENT_INST EQUAL -1)
        string(REPLACE "volume[i] = 0;\n\t\tSetDefaultInstrument(i);" "volume[i] = 0;\n\t\tcurrent_inst[i] = 0;\n\t\tSetDefaultInstrument(i);" MUS_CPP_CONTENT "${MUS_CPP_CONTENT}")
        string(REPLACE "if (timbre < nrTimbre && insts[timbre].backend_index >= 0)\n\t\t\t\t\tSetInstrument(voice, insts[timbre].backend_index);" "if (timbre < nrTimbre && insts[timbre].backend_index >= 0) {\n\t\t\t\t\tcurrent_inst[voice] = timbre;\n\t\t\t\t\tSetInstrument(voice, insts[timbre].backend_index);\n\t\t\t\t}" MUS_CPP_CONTENT "${MUS_CPP_CONTENT}")
        file(WRITE ${MUS_CPP_PATH} "${MUS_CPP_CONTENT}")
    endif()
endif()

# Patch adplug's sop.h and sop.cpp to expose the EXACT per-voice instrument, real-time volume, and attack peak events.
set(SOP_H_PATH "${adplug_SOURCE_DIR}/src/sop.h")
set(SOP_CPP_PATH "${adplug_SOURCE_DIR}/src/sop.cpp")

if(EXISTS ${SOP_H_PATH})
    file(READ ${SOP_H_PATH} SOP_H_CONTENT)
    string(FIND "${SOP_H_CONTENT}" "current_inst" HAS_SOP_CURRENT_INST)
    if(HAS_SOP_CURRENT_INST EQUAL -1)
        string(REPLACE "\tuint8_t nTracks, nInsts;" "\tuint8_t nTracks, nInsts;\n\tuint8_t current_inst[SOP_MAX_TRACK];\n\tuint8_t real_volume[SOP_MAX_TRACK];\n\tuint8_t real_attack[SOP_MAX_TRACK];" SOP_H_CONTENT "${SOP_H_CONTENT}")
        string(REPLACE "\t\treturn inst && n < nInsts ? std::string(inst[n].longname) : std::string();\n\t};" "\t\treturn inst && n < nInsts ? std::string(inst[n].longname) : std::string();\n\t};\n\n\tstd::string getvoiceinstrument(unsigned int voice)\n\t{\n\t\tif (!inst || voice >= (unsigned)nTracks) return std::string();\n\t\treturn current_inst[voice] < nInsts ? std::string(inst[current_inst[voice]].longname) : std::string();\n\t};\n\n\tuint8_t getvoicevolume(unsigned int voice)\n\t{\n\t\tif (voice >= (unsigned)nTracks) return 0;\n\t\treturn real_volume[voice];\n\t};\n\n\tuint8_t getvoicenote(unsigned int voice)\n\t{\n\t\tif (voice >= (unsigned)nTracks) return 60;\n\t\treturn drv ? drv->GetVoiceNote(voice) : 60;\n\t};\n\n\tuint8_t consumetrackattack(unsigned int voice)\n\t{\n\t\tif (voice >= (unsigned)nTracks) return 0;\n\t\tuint8_t val = real_attack[voice];\n\t\treal_attack[voice] = 0;\n\t\treturn val;\n\t};\n\n\tunsigned int getnumvoices() { return nTracks; };" SOP_H_CONTENT "${SOP_H_CONTENT}")
        string(REPLACE "int Set_4OP_Mode(unsigned chan, unsigned value);" "int Set_4OP_Mode(unsigned chan, unsigned value);\n\tchar GetVoiceNote(int chan) const { return (chan >= 0 && chan < 20) ? voiceNote[chan] : 60; }" SOP_H_CONTENT "${SOP_H_CONTENT}")
        file(WRITE ${SOP_H_PATH} "${SOP_H_CONTENT}")
    endif()
endif()

if(EXISTS ${SOP_CPP_PATH})
    file(READ ${SOP_CPP_PATH} SOP_CPP_CONTENT)
    string(FIND "${SOP_CPP_CONTENT}" "real_attack" HAS_SOP_ATTACK_PATCH)
    if(HAS_SOP_ATTACK_PATCH EQUAL -1)
        string(REPLACE "\t\tvolume[i] = 0;\n\t\tlastvol[i] = 0;\n\t}" "\t\tvolume[i] = 0;\n\t\tlastvol[i] = 0;\n\t\tcurrent_inst[i] = 0;\n\t\treal_volume[i] = 0;\n\t\treal_attack[i] = 0;\n\t}" SOP_CPP_CONTENT "${SOP_CPP_CONTENT}")
        string(REPLACE "\t\tif (t == nTracks || value >= nInsts) break;\n\t\tif (drv) drv->SetVoiceTimbre_SOP(t, inst[value].data);" "\t\tif (t == nTracks || value >= nInsts) break;\n\t\tcurrent_inst[t] = value;\n\t\tif (drv) drv->SetVoiceTimbre_SOP(t, inst[value].data);" SOP_CPP_CONTENT "${SOP_CPP_CONTENT}")
        string(REPLACE "if (drv) drv->NoteOn_SOP(t, value);" "if (drv) drv->NoteOn_SOP(t, value);\n\t\treal_volume[t] = volume[t] > 0 ? volume[t] : 64;\n\t\treal_attack[t] = volume[t] > 0 ? volume[t] : 64;" SOP_CPP_CONTENT "${SOP_CPP_CONTENT}")
        string(REPLACE "if (drv && !--track[i].dur) drv->NoteOff_SOP(i);" "if (drv && !--track[i].dur) {\n\t\t\tdrv->NoteOff_SOP(i);\n\t\t\treal_volume[i] = 0;\n\t\t\treal_attack[i] = 0;\n\t\t}" SOP_CPP_CONTENT "${SOP_CPP_CONTENT}")
        file(WRITE ${SOP_CPP_PATH} "${SOP_CPP_CONTENT}")
    endif()
endif()

# Patch adplug's rol.h and rol.cpp to expose exact per-voice instrument
set(ROL_H_PATH "${adplug_SOURCE_DIR}/src/rol.h")
set(ROL_CPP_PATH "${adplug_SOURCE_DIR}/src/rol.cpp")

if(EXISTS ${ROL_H_PATH})
    file(READ ${ROL_H_PATH} ROL_H_CONTENT)
    string(FIND "${ROL_H_CONTENT}" "getvoiceinstrument" HAS_ROL_GETTER)
    if(HAS_ROL_GETTER EQUAL -1)
        string(REPLACE "private:" "    std::string getvoiceinstrument(unsigned int voice) {\n        if (voice >= 16) return std::string();\n        unsigned int ins_idx = current_inst[voice];\n        return getinstrument(ins_idx);\n    };\n    unsigned int getnumvoices() { return mVoiceData.size(); };\n    uint8_t current_inst[16];\n\nprivate:" ROL_H_CONTENT "${ROL_H_CONTENT}")
        file(WRITE ${ROL_H_PATH} "${ROL_H_CONTENT}")
    endif()
endif()

if(EXISTS ${ROL_CPP_PATH})
    file(READ ${ROL_CPP_PATH} ROL_CPP_CONTENT)
    string(FIND "${ROL_CPP_CONTENT}" "current_inst" HAS_ROL_CPP_PATCH)
    if(HAS_ROL_CPP_PATCH EQUAL -1)
        string(REPLACE "mTimeOfLastNote    (0)\n{\n}" "mTimeOfLastNote    (0)\n{\n    memset(current_inst, 0, sizeof(current_inst));\n}" ROL_CPP_CONTENT "${ROL_CPP_CONTENT}")
        string(REPLACE "SetInstrument(voice, iEvents[voiceData.next_instrument_event].ins_index);" "if (voice < 16) current_inst[voice] = iEvents[voiceData.next_instrument_event].ins_index;\n                SetInstrument(voice, iEvents[voiceData.next_instrument_event].ins_index);" ROL_CPP_CONTENT "${ROL_CPP_CONTENT}")
        file(WRITE ${ROL_CPP_PATH} "${ROL_CPP_CONTENT}")
    endif()
endif()

# Patch adplug's composer.h and composer.cpp to expose getvoicevolume, consumetrackattack, mRealVolume, mRealAttack
set(COMPOSER_H_PATH "${adplug_SOURCE_DIR}/src/composer.h")
set(COMPOSER_CPP_PATH "${adplug_SOURCE_DIR}/src/composer.cpp")

if(EXISTS ${COMPOSER_H_PATH})
    file(READ ${COMPOSER_H_PATH} COMPOSER_H_CONTENT)
    string(FIND "${COMPOSER_H_CONTENT}" "getvoicevolume" HAS_COMP_GETTER)
    if(HAS_COMP_GETTER EQUAL -1)
        string(REPLACE "void SetDefaultInstrument(int const voice);" "void SetDefaultInstrument(int const voice);\n\n    virtual std::string getvoiceinstrument(unsigned int voice) { return std::string(); }\n    virtual uint8_t getvoicevolume(unsigned int voice) const { return voice < 16 ? mRealVolume[voice] : 0; }\n    virtual uint8_t consumetrackattack(unsigned int voice) {\n        if (voice >= 16) return 0;\n        uint8_t val = mRealAttack[voice];\n        mRealAttack[voice] = 0;\n        return val;\n    }\n    virtual unsigned int getnumvoices() const { return 16; }" COMPOSER_H_CONTENT "${COMPOSER_H_CONTENT}")
        string(REPLACE "static int   const kSnareNote;\n};" "static int   const kSnareNote;\n\nprotected:\n    uint8_t           mRealVolume[16];\n    uint8_t           mRealAttack[16];\n};" COMPOSER_H_CONTENT "${COMPOSER_H_CONTENT}")
        file(WRITE ${COMPOSER_H_PATH} "${COMPOSER_H_CONTENT}")
    endif()
endif()

if(EXISTS ${COMPOSER_CPP_PATH})
    file(READ ${COMPOSER_CPP_PATH} COMPOSER_CPP_CONTENT)
    string(FIND "${COMPOSER_CPP_CONTENT}" "mRealVolume" HAS_COMP_CPP_PATCH)
    if(HAS_COMP_CPP_PATCH EQUAL -1)
        string(REPLACE "mAMVibRhythmCache  (0)\n{\n}" "mAMVibRhythmCache  (0)\n{\n    std::memset(mRealVolume, 0, sizeof(mRealVolume));\n    std::memset(mRealAttack, 0, sizeof(mRealAttack));\n}" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        string(REPLACE "opl->write(skOPL2_WaveCtrlBaseAddress, skOPL2_EnableWaveformSelectMask); // Enable waveform select" "opl->write(skOPL2_WaveCtrlBaseAddress, skOPL2_EnableWaveformSelectMask); // Enable waveform select\n\n    std::memset(mRealVolume, 0, sizeof(mRealVolume));\n    std::memset(mRealAttack, 0, sizeof(mRealAttack));" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        string(REPLACE "void CcomposerBackend::NoteOn(int const voice, int const note)\n{\n    SetNote(voice, note + kSilenceNote);\n}" "void CcomposerBackend::NoteOn(int const voice, int const note)\n{\n    SetNote(voice, note + kSilenceNote);\n    if (voice >= 0 && voice < 16) {\n        mRealVolume[voice] = mVolumeCache[voice] > 0 ? mVolumeCache[voice] : 64;\n        mRealAttack[voice] = mRealVolume[voice];\n    }\n}" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        string(REPLACE "void CcomposerBackend::NoteOff(int const voice)\n{\n    SetNote(voice, kSilenceNote);\n}" "void CcomposerBackend::NoteOff(int const voice)\n{\n    SetNote(voice, kSilenceNote);\n    if (voice >= 0 && voice < 16) {\n        mRealVolume[voice] = 0;\n        mRealAttack[voice] = 0;\n    }\n}" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        string(REPLACE "mVolumeCache[voice] = volume;\n\n    opl->write(skOPL2_KSLTLBaseAddress + op_offset, GetKSLTL(voice));" "mVolumeCache[voice] = volume;\n    if (voice >= 0 && voice < 16 && mRealVolume[voice] > 0) {\n        mRealVolume[voice] = volume;\n    }\n\n    opl->write(skOPL2_KSLTLBaseAddress + op_offset, GetKSLTL(voice));" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        file(WRITE ${COMPOSER_CPP_PATH} "${COMPOSER_CPP_CONTENT}")
    endif()
endif()

# Reset the rhythm register cache in CcomposerBackend::rewind().
#
# rewind() resets every other cache and calls opl->init(), but it leaves
# mAMVibRhythmCache alone. Once a song has hit a drum, that cache still holds
# the drum key-on bits, so the next SetRhythmMode(1) writes them straight back
# to the just-reset chip: 0xBD gets 0x3B (rhythm on + cymbal + hi-hat + snare +
# bass drum) instead of 0x20, and all four drums fire at once. That is the loud
# "칭~" heard when the SAME song is stopped and played again, while switching
# songs was always clean - switching builds a new player object, whose
# constructor zeroes the cache. Diagnosed 2026-07-27 by rendering ZOOM01.IMS
# twice offline and diffing the emulated chip state; the pending 0xBD write was
# the only difference. mRhythmMode is reset too, since init() has just put the
# chip back in melodic mode.
if(EXISTS ${COMPOSER_CPP_PATH})
    file(READ ${COMPOSER_CPP_PATH} COMPOSER_CPP_CONTENT)
    string(FIND "${COMPOSER_CPP_CONTENT}" "mAMVibRhythmCache = 0;" HAS_COMP_RHYTHM_RESET)
    if(HAS_COMP_RHYTHM_RESET EQUAL -1)
        string(REPLACE "opl->init();         // initialize to melodic by default" "opl->init();         // initialize to melodic by default\n    mAMVibRhythmCache = 0;   // drop drum key-on bits left by the previous run\n    mRhythmMode       = 0;   // init() put the chip back in melodic mode" COMPOSER_CPP_CONTENT "${COMPOSER_CPP_CONTENT}")
        file(WRITE ${COMPOSER_CPP_PATH} "${COMPOSER_CPP_CONTENT}")
    endif()
endif()

# Source files
set(SOURCES
    main.cpp
    mainwindow.cpp
    midiplayer.cpp
    channelmonitor.cpp
    lyricswindow.cpp
    lyricswindow_editor.cpp
    mainwindow_playlist.cpp
    mainwindow_lyrics.cpp
    mainwindow_playback.cpp
    settingsmanager.cpp
    nobfilehandler.cpp
    gybfilehandler.cpp
    okafilehandler.cpp
    okabackend.cpp
    okaplayer.cpp
    gybplayer.cpp
    gybbackend.cpp
    jjomesynth.cpp
    soundfontmanagerdialog.cpp
    imsplayer.cpp
    issfilehandler.cpp
    folderscanner.cpp
    pianorollwindow.cpp
    playlistmodel.cpp
    oplstereodialog.cpp
    opltunnelsender.cpp
    midireset/midireset.cpp
    midireset/midiresetdialog.cpp
)

set(HEADERS
    mainwindow.h
    midiplayer.h
    channelmonitor.h
    lyricswindow.h
    constants.h
    settingsmanager.h
    nobfilehandler.h
    gybfilehandler.h
    okafilehandler.h
    okabackend.h
    okaplayer.h
    gybplayer.h
    jjomesynth.h
    soundfontmanagerdialog.h
    imsplayer.h
    issfilehandler.h
    folderscanner.h
    pianorollwindow.h
    playlistmodel.h
    oplstereodialog.h
)

# Resource files
set(RESOURCES
    resources.qrc
)

# Windows resource file for icon
if(WIN32)
    list(APPEND RESOURCES app.rc)
endif()

# UI files (none for this project - using code-only UI)
# set(UI_FILES
#     mainwindow.ui
# )

# Create executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} ${RESOURCES})

# Link Qt6 libraries
target_link_libraries(${PROJECT_NAME}
    Qt6::Core
    Qt6::CorePrivate
    Qt6::GuiPrivate
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
    adplug
    winmm
    dwmapi
)

# English UI build: pass -DENGLISH_UI=ON to build all player UI text in English.
# (Song lyrics are NOT affected — only the player's own interface text.)
option(ENGLISH_UI "Build the player UI in English" OFF)
if(ENGLISH_UI)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ENGLISH_UI)
    message(STATUS "ENGLISH_UI = ON  (English player UI build)")
endif()

# Windows specific
if(WIN32)
    # set_target_properties(${PROJECT_NAME} PROPERTIES
    #     WIN32_EXECUTABLE TRUE
    # )
endif()

# Copy SoundFonts directory to build output
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/SoundFonts"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SoundFonts"
    COMMENT "Copying SoundFonts to output directory"
)

# Copy BK directory to build output
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/BK"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/BK"
    COMMENT "Copying Sample songs to output directory"
)

# Copy STANDARD.BNK next to the executable so dev builds
# match the same bank the IMS player uses (via ImsFileProvider).
if(EXISTS "${CMAKE_SOURCE_DIR}/IMS/STANDARD.BNK")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/IMS/STANDARD.BNK"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/STANDARD.BNK"
        COMMENT "Copying STANDARD.BNK for GybPlayer"
    )
elseif(EXISTS "${CMAKE_SOURCE_DIR}/dos/STANDARD.BNK")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/dos/STANDARD.BNK"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/STANDARD.BNK"
        COMMENT "Copying STANDARD.BNK for GybPlayer"
    )
endif()
