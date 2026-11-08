cmake_minimum_required(VERSION 3.11)

project(devilishpsp)

# Always optimise: psp-cmake leaves CMAKE_BUILD_TYPE empty, which means -O0. -O2 for every build type.
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_C_FLAGS${cfg} "${CMAKE_C_FLAGS${cfg}} -O2")
    set(CMAKE_CXX_FLAGS${cfg} "${CMAKE_CXX_FLAGS${cfg}} -O2")
endforeach()

# The translated game (tools/z80_to_c.py -> extracted/gen/*.c, local and git-ignored), the runtime that holds it together and the hardware model.
set(GEN_DIR ${CMAKE_SOURCE_DIR}/extracted/gen)
set(GEN_SOURCES ${GEN_DIR}/mod_lo.c ${GEN_DIR}/mod_hi.c ${GEN_DIR}/mod_b2.c ${GEN_DIR}/gen_info.c ${GEN_DIR}/rom_data.c)

# Sound: the game unpacks a Z80 sound driver into RAM at boot; like the rest of the game it is translated from the ROM (module ram, extracted/gen/mod_ram.c).
list(APPEND GEN_SOURCES ${GEN_DIR}/mod_ram.c)
foreach(f ${GEN_SOURCES})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "${f} is missing: the game code is generated from your own ROM, run the kit's build script (see README) or python tools/z80_to_c.py and python tools/embed_rom.py")
    endif()
endforeach()
# generated code is not meant to be warning-clean; the big modules take a couple of minutes each to compile
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_FLAGS "-w")

add_executable(${PROJECT_NAME}
    src/psp/main.c
    src/psp/gerender.c
    src/psp/config.c
    src/rt/runtime.c
    src/rt/hiscore.c
    src/rt/state.c
    src/hw/hw.c
    src/hw/scenedraw.c
    src/hw/psg.c
    ${GEN_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(${PROJECT_NAME} PRIVATE HW_SCENE HW_NO_PIXELS)      # the scene recorder and draw-list builder (src/hw/scene.inc, scenedraw.inc) feed the GE; the CPU line renderer is compiled out (V5): the sprite flags come from sprite_flags() in hw.c
target_compile_options(${PROJECT_NAME} PRIVATE -O2 -G0 -Wall -fno-strict-aliasing)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pspgu
    pspdebug
    pspdisplay
    pspge
    pspctrl
    psppower
    pspaudio
    m
)

# Optional XMB art lives in pbp/ (ICON0.png, PIC0.png, PIC1.png, SND0.AT3); missing files are skipped.
function(pbp_file out_var filename)
    if(EXISTS ${CMAKE_SOURCE_DIR}/pbp/${filename})
        set(${out_var} ${CMAKE_SOURCE_DIR}/pbp/${filename} PARENT_SCOPE)
    else()
        set(${out_var} NULL PARENT_SCOPE)
    endif()
endfunction()

pbp_file(PBP_ICON ICON0.png)
pbp_file(PBP_PREVIEW PIC0.png)
pbp_file(PBP_BACKGROUND PIC1.png)
pbp_file(PBP_MUSIC SND0.AT3)

create_pbp_file(
    TARGET ${PROJECT_NAME}
    ICON_PATH ${PBP_ICON}
    BACKGROUND_PATH ${PBP_BACKGROUND}
    PREVIEW_PATH ${PBP_PREVIEW}
    MUSIC_PATH ${PBP_MUSIC}
    TITLE "Devilish PSP"
    VERSION 01.00
    # MEMSIZE 1 unlocks the extra RAM on PSP-2000/3000 (create_pbp_file defaults to 2 = the standard partition).
    MEMSIZE 1
)
